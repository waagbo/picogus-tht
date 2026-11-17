// lwIP options for the host test: the firmware's options (same heap size,
// pbuf size and TCP windows), with the heap in host memory and a loopback
// interface instead of the WiFi chip.
#include "../lwipopts.h"

#undef LWIP_RAM_HEAP_POINTER
extern unsigned char test_lwip_heap[];
#define LWIP_RAM_HEAP_POINTER test_lwip_heap

#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     0

// routing to the loopback interface needs the multi-netif code
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF           0

// 64 bit host
#undef MEM_ALIGNMENT
#define MEM_ALIGNMENT               8
