// lwIP configuration for the PicoGUS web server.
//
// Tuned for a small footprint: the network stack shares the RP2040's RAM with
// the sound card emulation (all code runs from RAM, PICO_COPY_TO_RAM=1), so
// buffers are kept small. Small TCP windows also bound how much work a single
// poll of the network stack can do, which keeps it from starving audio.

#ifndef _PICOGUS_LWIPOPTS_H
#define _PICOGUS_LWIPOPTS_H

// Bare metal, polled from the core 1 main loop
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        0

// Memory: everything comes from one lwIP heap in the XIP cache SRAM, which
// PicoGUS doesn't otherwise use (see web_xip_sram.h), so the network costs no
// main RAM for buffers. Received frames are stored in chains of small pbufs to
// make good use of it.
#include "web_xip_sram.h"
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4
#ifdef WEB_XIP_LWIP_BASE
#define LWIP_RAM_HEAP_POINTER       ((void *)WEB_XIP_LWIP_BASE)
#endif
#define MEM_SIZE                    (WEB_XIP_LWIP_SIZE - 64)
#define PBUF_POOL_BUFSIZE           512
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     1
#define MEMP_NUM_UDP_PCB            3

// Protocols
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    0
#define LWIP_IGMP                   PICOGUS_WEB_MDNS
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_SINGLE_NETIF           1
#define IP_REASSEMBLY               0
#define IP_FRAG                     0

// TCP: small windows keep RAM use and per-poll work low. A web UI does not
// need more throughput than this.
#define TCP_MSS                     1460
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_QUEUE_OOSEQ             0
#define TCP_LISTEN_BACKLOG          0
#define LWIP_TCP_KEEPALIVE          0
#define LWIP_TCP_TIMESTAMPS         0
#define LWIP_TCP_SACK_OUT           0
// The server closes each connection, which keeps it in TIME_WAIT for 2 * MSL.
// Those PCBs come out of the small heap, so keep TIME_WAIT short (2s) - plenty
// on a local network - or the UI's status polling would tie up the heap.
#define TCP_MSL                     1000

// Random numbers for TCP sequence numbers, DHCP transaction IDs and ports.
// Not the SDK's pico_rand: its state lands in .uninitialized_data, which the
// multifw linker script places differently from the one used to size the
// image, making the firmwares not fit their slots in flash.
#ifdef __cplusplus
extern "C"
#endif
unsigned int web_rand(void);
#define LWIP_RAND()                 web_rand()

// netif
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3

// mDNS ("picogus.local")
#if PICOGUS_WEB_MDNS
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MDNS_MAX_SERVICES           1
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3)
#endif

// No statistics, debug output or asserts: release builds of PicoGUS strip
// printf entirely and fail the build if anything still calls it.
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
// LWIP_DEBUG must stay undefined (lwIP tests it with #ifdef)
#define LWIP_NOASSERT               1

#endif // _PICOGUS_LWIPOPTS_H
