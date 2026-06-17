#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwIP runs NO_SYS over the TinyUSB NCM interface; everything is serviced
// from the single main loop (usb_net_task), so no OS/locking is needed.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Footprint is kept DELIBERATELY SMALL. This stack serves one ~5 KB config page
// over a single short-lived HTTP connection at USB speed; it is NEVER a
// throughput path. lwIP is always up here (no time-share), so every byte of its
// static footprint permanently shrinks the heap shared with BTstack (~40 KB at
// boot) and the Opus codec runtime (~76 KB on controller-connect). The earlier
// web-scale sizing (MEM_SIZE 8000, PBUF_POOL 8 x 1460 B, 8xMSS windows) cost
// ~21 KB of BSS and OOM-panicked Opus the moment a controller connected. These
// values are the minimum that still streams the page: a small MSS keeps each
// pbuf small, and over the low-latency USB link the extra round-trips are free.
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    1600
#define MEMP_NUM_TCP_SEG            14  // must be >= TCP_SND_QUEUELEN (see below)
#define MEMP_NUM_ARP_QUEUE          2
#define MEMP_NUM_UDP_PCB            3
#define MEMP_NUM_TCP_PCB            5   // active conns + a couple lingering TIME_WAIT
#define MEMP_NUM_TCP_PCB_LISTEN     1   // single httpd listener
#define MEMP_NUM_PBUF               4
#define PBUF_POOL_SIZE              4   // 4 * ~600 B (small MSS) ~= 2.4 KB
#define TCP_MSL                     1000  // ms (default 60000); short TIME_WAIT linger

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_DHCP                   0   // we are the DHCP *server* (dhserver.c, raw UDP)
#define LWIP_DNS                    0   // deliberately no DNS: never hijack host lookups
#define LWIP_IGMP                   0   // no multicast: mDNS removed (never resolved here, see below)

// Let ip4_input accept link-layer-addressed packets (src 0.0.0.0) destined
// for UDP port 67: required for the DHCP *server* to see client DISCOVERs.
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))

// Small MSS keeps each pbuf-pool buffer small (PBUF_POOL_BUFSIZE tracks MSS), so
// the pool costs little BSS. The ~5 KB page streams across many small segments;
// over the low-latency USB link the extra round-trips are invisible.
#define TCP_MSS                     536
#define TCP_WND                     (4 * TCP_MSS)   // ~2.1 KB receive window
#define TCP_SND_BUF                 (3 * TCP_MSS)   // ~1.6 KB; QUEUELEN ~13, fits SEG=14
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

// mDNS responder removed: without NETIF_FLAG_IGMP it could never join the
// multicast group, so ds5config.local never resolved here -- and enabling IGMP
// faulted this NCM setup into a watchdog reboot loop. Users reach the page by IP
// (http://10.55.55.105/). Dropping it also frees the EXT_STATUS_CALLBACK +
// NETIF_CLIENT_DATA machinery and the pico_lwip_mdns library.

// HTTP server: all content is generated in fs_open_custom / the POST hooks
// (usb_net.cpp); the static fsdata table is empty (pico_fsdata.inc).
#define LWIP_HTTPD_CUSTOM_FILES     1
#define LWIP_HTTPD_DYNAMIC_HEADERS  0     // responses carry their own headers
#define LWIP_HTTPD_SUPPORT_POST     1
#define LWIP_HTTPD_SSI              0
#define LWIP_HTTPD_CGI              0
#define HTTPD_FSDATA_FILE           "pico_fsdata.inc"

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#define LWIP_CHKSUM_ALGORITHM       3

#endif /* LWIPOPTS_H */
