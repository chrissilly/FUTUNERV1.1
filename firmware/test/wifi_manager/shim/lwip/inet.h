#ifndef HOST_SHIM_LWIP_INET_H
#define HOST_SHIM_LWIP_INET_H

#include <stdint.h>

/* These macros only feed into snprintf calls in code paths the host test
 * never exercises (the boot-time AP-init logger lines). Provide a
 * functional shape so the format strings compile. */
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ipaddr) \
    (unsigned)(((ipaddr)->addr      ) & 0xff), \
    (unsigned)(((ipaddr)->addr >>  8) & 0xff), \
    (unsigned)(((ipaddr)->addr >> 16) & 0xff), \
    (unsigned)(((ipaddr)->addr >> 24) & 0xff)

#endif /* HOST_SHIM_LWIP_INET_H */
