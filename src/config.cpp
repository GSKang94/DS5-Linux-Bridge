//
// Created by awalol on 2026/5/4.
//

#include "config.h"

#include <cmath>
#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h" // flash_safe_execute(): park core1 during the flash op
#include "usb_net.h" // WEBCONFIG_SUBNET_COUNT (subnet-index bound, always defined)
#include "utils.h"

constexpr uint32_t CONFIG_MAGIC = 0x66ccff00;
constexpr uint16_t CONFIG_VERSION = 5; // v5 added webconfig_custom_ip; v4 bond_names
constexpr uint32_t CONFIG_FLASH_OFFSET =
    PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
// flash_safe_execute() timeout per attempt, and how many times we retry when
// core1 (audio) fails to park in time. Total worst-case block ~= product of the
// two; kept modest so a wedged core1 can't stall the web request indefinitely.
constexpr uint32_t CONFIG_SAVE_TIMEOUT_MS = 1000;
constexpr int CONFIG_SAVE_RETRIES = 3;
static Config config{};
bool is_dse = false;

// 编译期保护
// 判断Config结构体是否能放进flash 256bytes
static_assert(sizeof(Config) <= FLASH_PAGE_SIZE);
// 配置区起始地址必须按 flash sector 对齐。
static_assert(CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

uint32_t calc_config_crc(const Config &con) {
  return crc32(reinterpret_cast<const uint8_t *>(&con.body),
               sizeof(Config_body));
}

const Config *flash_config() {
  return reinterpret_cast<const Config *>(XIP_BASE + CONFIG_FLASH_OFFSET);
}

void config_valid() {
  // valid config and set default value
  if (config.magic != CONFIG_MAGIC) {
    config.magic = CONFIG_MAGIC;
    printf("[Config] Config Magic Header is invalid\n");
  }
  if (config.version != CONFIG_VERSION) {
    config.version = CONFIG_VERSION;
    printf("[Config] Config Version is invalid\n");
  }
  if (config.size != sizeof(Config_body)) {
    config.size = sizeof(Config_body);
    printf("[Config] Config Body size is invalid\n");
  }
  auto body = &config.body;
  if (std::isnan(body->speaker_volume) || body->speaker_volume < -100 ||
      body->speaker_volume > 0) {
    body->speaker_volume = -100;
    printf("[Config] Speaker Volume is invalid\n");
  }
  if (body->inactive_time < 5 || body->inactive_time > 60) {
    body->inactive_time = 30;
    printf("[Config] Inactive time is invalid\n");
  }
  if (body->disable_inactive_disconnect > 1) {
    body->disable_inactive_disconnect = 0;
    printf("[Config] disable_auto_disconnect is invalid\n");
  }
  if (body->disable_pico_led > 1) {
    body->disable_pico_led = 0;
    printf("[Config] disable_pico_led is invalid\n");
  }
  if (body->polling_rate_mode > 2) {
    body->polling_rate_mode = 2;
    printf("[Config] polling_rate_mode is invalid\n");
  }
  if (body->audio_buffer_length < 16 || body->audio_buffer_length > 128) {
    body->audio_buffer_length = 64;
    printf("[Config] haptics_buffer_length is invalid\n");
  }
  if (body->controller_mode > 2) {
    body->controller_mode = 2;
    printf("[Config] controller_mode is invalid\n");
  }
  if (body->webconfig_subnet > WEBCONFIG_SUBNET_MAX) {
    body->webconfig_subnet = 0; // default: 10.55.55.x
    printf("[Config] webconfig_subnet is invalid\n");
  }
  // If "custom IP" is selected, the stored address must be a valid private host
  // address; otherwise fall back to the default preset so the page stays
  // reachable (this is the safety net behind the custom-IP YOLO option).
  if (body->webconfig_subnet == WEBCONFIG_SUBNET_CUSTOM &&
      !webconfig_ip_is_valid(body->webconfig_custom_ip)) {
    body->webconfig_subnet = 0;
    printf("[Config] webconfig_custom_ip invalid; using default preset\n");
  }
  if (body->config_version != CONFIG_VERSION) {
    body->config_version = CONFIG_VERSION;
    printf("[Config] Warning: Config may breaking change\n");
  }
  // Defensive: guarantee every bond name is NUL-terminated so corrupt flash
  // can never yield an unbounded C string when the web UI reads it.
  for (auto &b : body->bond_names) {
    b.name[CONFIG_BOND_NAME_LEN - 1] = '\0';
  }
}

void config_load() {
  memcpy(&config, flash_config(), sizeof(Config));

  // Migration: on any version mismatch (older format, or uninitialized/0xFF
  // flash) the trailing struct region is garbage. v5 inserted webconfig_custom_ip
  // ahead of bond_names, shifting their offsets, so zero both: the custom IP
  // becomes 0.0.0.0 (invalid -> config_valid() falls back to the default preset)
  // and nicknames start empty rather than random. config_valid() (below) then
  // bumps the version and clamps every other field.
  if (config.version != CONFIG_VERSION) {
    memset(config.body.webconfig_custom_ip, 0, sizeof(config.body.webconfig_custom_ip));
    memset(config.body.bond_names, 0, sizeof(config.body.bond_names));
  }

  config_valid();
}

// Runs with core1 parked (flash_safe_execute) and core0 interrupts disabled, so
// neither core touches XIP flash while the sector is erased/programmed. Without
// the core1 park this races the audio core and corrupts audio (buzzing) -- or
// faults -- when saving during playback (e.g. bond rename while connected).
static void config_save_flash_op(void *param) {
  const uint8_t *page = static_cast<const uint8_t *>(param);
  const uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(CONFIG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
  restore_interrupts(interrupts);
}

bool config_save() {
  config.crc32 = calc_config_crc(config);
  alignas(4) uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xff, sizeof(page));
  memcpy(page, &config, sizeof(Config));

  // flash_safe_execute() parks core1 (the audio core) before touching flash. If
  // core1 is asleep in __wfe() (idle audio loop) it can miss the lockout request
  // and the call returns PICO_ERROR_TIMEOUT -- the erase/program never happens.
  // Historically the single failure was ignored by callers, so the web UI would
  // report "saved" (RAM was updated) while flash kept the old bytes; the change
  // then vanished on the next boot. Retry a few times, nudging core1 awake with
  // __sev() before each attempt so its flash-safe IRQ handler can run.
  int rc = PICO_ERROR_TIMEOUT;
  for (int attempt = 0; attempt < CONFIG_SAVE_RETRIES; attempt++) {
    __sev(); // wake core1 out of __wfe() so it can honour the flash-safe lockout
    rc = flash_safe_execute(config_save_flash_op, page, CONFIG_SAVE_TIMEOUT_MS);
    if (rc == PICO_OK) break;
    printf("[Config] config_save flash_safe_execute failed (attempt %d/%d): %d\n",
           attempt + 1, CONFIG_SAVE_RETRIES, rc);
  }
  if (rc != PICO_OK) {
    printf("[Config] config_save FAILED after %d attempts: %d (config NOT persisted)\n",
           CONFIG_SAVE_RETRIES, rc);
    return false;
  }

  Config verify{};
  memcpy(&verify, flash_config(), sizeof(verify));
  const auto verify_crc32 = calc_config_crc(verify);
  if (verify_crc32 == config.crc32) {
    printf("[Config] Config write flash verify success\n");
    return true;
  }
  printf("[Config] Config write flash verify FAILED (config NOT persisted)\n");
  return false;
}

const Config_body &get_config() { return config.body; }

void set_config(const uint8_t *new_config, const uint16_t len) {
  const auto copy_len = len < sizeof(Config_body) ? len : sizeof(Config_body);
  memcpy(&config.body, new_config, copy_len);
  config_valid();
  if (config.body.disable_pico_led) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
  } else {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
  }
}

//--------------------------------------------------------------------+
// Bond-name table helpers. An all-zero addr marks an empty slot.
//--------------------------------------------------------------------+

static bool addr_is_zero(const uint8_t *a) {
  for (int i = 0; i < CONFIG_BOND_ADDR_LEN; i++)
    if (a[i]) return false;
  return true;
}

static bool addr_eq(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, CONFIG_BOND_ADDR_LEN) == 0;
}

const char *config_bond_name(const uint8_t *addr) {
  if (!addr) return nullptr;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) return b.name;
  }
  return nullptr;
}

void config_clear_bond_name(const uint8_t *addr) {
  if (!addr) return;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) {
      memset(&b, 0, sizeof(b));
    }
  }
}

bool config_set_bond_name(const uint8_t *addr, const char *name) {
  if (!addr || addr_is_zero(addr)) return false;
  if (!name) name = "";

  // Blank/whitespace-only name clears the slot instead of storing it.
  bool blank = true;
  for (const char *p = name; *p; p++)
    if (*p != ' ' && *p != '\t') { blank = false; break; }
  if (blank) {
    config_clear_bond_name(addr);
    return true;
  }

  BondName *slot = nullptr;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) { slot = &b; break; }
  }
  if (!slot) {
    for (auto &b : config.body.bond_names) {
      if (addr_is_zero(b.addr)) { slot = &b; break; }
    }
  }
  if (!slot) return false; // table full

  memcpy(slot->addr, addr, CONFIG_BOND_ADDR_LEN);
  strncpy(slot->name, name, CONFIG_BOND_NAME_LEN - 1);
  slot->name[CONFIG_BOND_NAME_LEN - 1] = '\0';
  return true;
}

void set_config(const Config_body &new_config) {
  config.body = new_config;
  config_valid();
  // Apply the live-effective LED state immediately (mirrors the uint8_t*
  // overload). Other live fields (inactive_time, audio_buffer_length, ...) are
  // re-read on their own cadence; descriptor-bound fields (controller_mode,
  // polling_rate_mode) only take effect on the next USB re-enumeration.
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !config.body.disable_pico_led);
}
