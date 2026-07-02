//
// bootsel_button.h -- read the BOOTSEL button at runtime (RP2350).
//
// The board has no spare user button, so BOOTSEL doubles as the WiFi re-onboard
// trigger: held during the early-boot window, it forces the WiFi-WOL build into
// AP + captive portal so the user can re-enter credentials without reflashing.
//
// Reading BOOTSEL means briefly taking over the QSPI CS pad (it is shared with
// the flash chip-select) and sampling it as a GPIO input. That requires PAUSING
// execute-in-place: the helper MUST live in RAM (__no_inline_not_in_flash_func)
// and runs with interrupts disabled, because while XIP is paused any code/data
// fetch from flash would fault. This is the canonical pico-examples
// (picoboard/button) routine, adapted to RP2350's IO_QSPI layout.
//
// CONTRACT: call ONLY very early in main() -- after board_init() but BEFORE
// cyw43_arch_init(), BT, audio (core1), and the watchdog. At that point nothing
// latency-critical runs from flash, so the XIP pause is harmless; calling it
// once audio/BT are live could glitch them.
//

#ifndef DS5_BRIDGE_BOOTSEL_BUTTON_H
#define DS5_BRIDGE_BOOTSEL_BUTTON_H

#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/types.h"

// True while the BOOTSEL button is physically pressed. Must run from RAM with
// interrupts off (XIP is paused). See header note for the call-site contract.
static bool __no_inline_not_in_flash_func(bootsel_button_pressed)(void) {
    const uint CS_PIN_INDEX = 1; // QSPI SS is the flash chip-select pad

    const uint32_t flags = save_and_disable_interrupts();

    // Drive the CS pin as a SIO input by switching its dormant function to GPIO
    // (OEOVER = disable output) and reading the pad. Restore the original
    // function afterward so XIP can resume.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // The pin is pulled high externally; the button shorts it to ground. Wait a
    // few microseconds for the input to settle after taking over the pad.
    for (volatile int i = 0; i < 1000; ++i);

    // BOOTSEL is pressed when CS reads LOW (0).
    const bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    // Restore CS to its normal (XIP) function.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

#endif // DS5_BRIDGE_BOOTSEL_BUTTON_H
