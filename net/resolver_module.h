/*
 * resolver_module.h — PhoenixResolver: DNS stub resolver
 * boot379: Implements the Inet6Sources Resolver SWI chunk 0x46000.
 *
 * SWI layout mirrors Inet6Sources/ResolverLib/h/resolver6_swi:
 *   0x46000  Resolver_GetHostByName
 *   0x46001  Resolver_GetHost         (future)
 *   0x46002  Resolver_CacheControl    (future)
 *   0x46004  Resolver_GetAddrInfo     (future)
 *
 * Author: R Andrews – boot379
 */

#ifndef RESOLVER_MODULE_H
#define RESOLVER_MODULE_H

#include <stdint.h>

/* ── SWI chunk ─────────────────────────────────────────────────────────── */
#define RESOLVER_SWI_BASE           0x46000u

#define RESOLVER_SWI_GETHOSTBYNAME  0u   /* Resolver_GetHostByName  */
#define RESOLVER_SWI_GETHOST        1u   /* Resolver_GetHost        */
#define RESOLVER_SWI_CACHECONTROL   2u   /* Resolver_CacheControl   */
#define RESOLVER_SWI_GETADDRINFO    4u   /* Resolver_GetAddrInfo    */

/* ── hostent — BSD standard layout ─────────────────────────────────────── */
typedef struct hostent {
    char     *h_name;        /* official host name                   */
    char    **h_aliases;     /* alias list (NULL-terminated)         */
    int       h_addrtype;    /* host address type (AF_INET = 2)      */
    int       h_length;      /* length of address (4 for IPv4)       */
    char    **h_addr_list;   /* list of addresses, NULL-terminated   */
} hostent_t;

#define AF_INET   2

/* ── Public API ─────────────────────────────────────────────────────────── */
int        resolver_module_init  (void);
int        resolver_module_final (void);
int        resolver_module_swi   (uint32_t swi_offset, uint32_t *regs);
hostent_t *resolver_gethostbyname(const char *name);

#endif /* RESOLVER_MODULE_H */
