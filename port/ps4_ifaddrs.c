/* WinePS4: the network interfaces (getifaddrs, if_nameindex, ...) from the PS4's kernel.
 *
 * The C library's come from Linux's musl: they ask a netlink socket, which a FreeBSD kernel doesn't
 * have, so they found nothing (if_nameindex() gave NULL). Wine's network driver, nsiproxy, lists
 * the interfaces and their addresses with them, for GetAdaptersAddresses and the TCP table: without
 * it every program had "GetAdaptersAddresses failed: 2", and Steam rejected its own web helper's
 * connection, whose owner it looks up in the TCP table (v00.51). FreeBSD's SIOCGIFCONF lists every
 * address of every interface, the link level one (sockaddr_dl: index, type, MAC) first. The C
 * library's headers have Linux's numbers for these ioctls: FreeBSD's are here, made with the raw
 * system call (a negative int request is sign-extended, and the kernel logs every one). */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define FBSD_SIOCGIFCONF    0xc0106924ul  /* _IOWR('i', 36, struct ifconf) */
#define FBSD_SIOCGIFFLAGS   0xc0206911ul  /* _IOWR('i', 17, struct ifreq) */
#define FBSD_SIOCGIFBRDADDR 0xc0206923ul  /* _IOWR('i', 35, struct ifreq) */
#define FBSD_SIOCGIFNETMASK 0xc0206925ul  /* _IOWR('i', 37, struct ifreq) */

#define FBSD_IFF_BROADCAST  0x2
#define FBSD_IFF_MULTICAST  0x8000
#define LIBC_IFF_MULTICAST  0x1000  /* (the C library's headers: Linux's) */
#define FBSD_IFT_LOOP       0x18

struct fbsd_ifreq  /* 32 bytes, as FreeBSD's on amd64 */
{
    char name[16];
    union
    {
        struct sockaddr addr;
        short flags[2];  /* low and high 16 bits */
        char size[16];
    } u;
};

struct fbsd_ifconf
{
    int len;
    char *buf;
};

struct fbsd_sockaddr_dl
{
    unsigned char len, family;
    unsigned short index;
    unsigned char type, nlen, alen, slen;
    char data[46];
};

int ps4_ioctl( int fd, unsigned long request, void *arg )
{
    long ret;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "=a" (ret), "=qm" (failed)
                      : "a" (54L /* SYS_ioctl */), "D" ((long)fd), "S" (request), "d" (arg)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (failed)
    {
        errno = (int)ret;
        return -1;
    }
    return (int)ret;
}

/* --- The kernel's list -------------------------------------------------------------------------- */

#define MAX_IFS     16
#define IFCONF_SIZE 16384

struct ps4_if
{
    char name[16];
    unsigned int index;
    unsigned char type, alen, addr[16];  /* the link level's (sockaddr_dl) */
};

/* every address of every interface (SIOCGIFCONF) into buf: its length, or -1. An entry is a whole
 * ifreq (32 bytes) when its address fits in a struct sockaddr, else the name and the address as
 * long as it is (FreeBSD's ifconf). */
static int read_ifconf( int sock, char *buf, int size )
{
    struct fbsd_ifconf conf = { size, buf };

    if (ps4_ioctl( sock, FBSD_SIOCGIFCONF, &conf ) < 0) return -1;
    return conf.len;
}

static int entry_size( const struct sockaddr *sa )
{
    return sa->sa_len <= sizeof(struct sockaddr) ? (int)sizeof(struct fbsd_ifreq) : 16 + sa->sa_len;
}

/* the interfaces, in the kernel's order: how many */
static int list_ifs( struct ps4_if *ifs, int max )
{
    char *buf = malloc( IFCONF_SIZE );  /* (each call its own: several threads may ask) */
    int sock = socket( AF_INET, SOCK_DGRAM, 0 ), len = -1, pos, n = 0, i;

    if (sock >= 0 && buf) len = read_ifconf( sock, buf, IFCONF_SIZE );
    if (sock >= 0) close( sock );
    for (pos = 0; len > 0 && pos + (int)sizeof(struct fbsd_ifreq) <= len; )
    {
        const struct fbsd_ifreq *req = (const struct fbsd_ifreq *)(buf + pos);
        const struct sockaddr *sa = &req->u.addr;

        for (i = 0; i < n && strncmp( ifs[i].name, req->name, 16 ); i++) ;
        if (i == n && n < max)
        {
            memset( &ifs[n], 0, sizeof(ifs[n]) );
            memcpy( ifs[n].name, req->name, 15 );
            ifs[n].index = n + 1;
            n++;
        }
        if (i < n && sa->sa_family == AF_LINK)
        {
            const struct fbsd_sockaddr_dl *dl = (const struct fbsd_sockaddr_dl *)sa;
            if (dl->index) ifs[i].index = dl->index;
            ifs[i].type = dl->type;
            if (dl->alen <= sizeof(ifs[i].addr) && dl->nlen + dl->alen <= sizeof(dl->data))
            {
                ifs[i].alen = dl->alen;
                memcpy( ifs[i].addr, dl->data + dl->nlen, dl->alen );
            }
        }
        pos += entry_size( sa );
    }
    free( buf );
    return n;
}

/* the link level of interface name (nsiproxy's ndis.c): 1 with its type (IFT_*) and address */
int ps4_if_link_info( const char *name, unsigned char *type, unsigned char *addr, unsigned int *len )
{
    struct ps4_if ifs[MAX_IFS];
    int n = list_ifs( ifs, MAX_IFS ), i;

    for (i = 0; i < n; i++)
    {
        if (strcmp( ifs[i].name, name )) continue;
        *type = ifs[i].type;
        if (!*type && !strncmp( name, "lo", 2 )) *type = FBSD_IFT_LOOP;
        *len = ifs[i].alen;
        memcpy( addr, ifs[i].addr, ifs[i].alen );
        return 1;
    }
    return 0;
}

/* --- The C library's calls -------------------------------------------------------------------- */

struct if_nameindex *if_nameindex( void )
{
    struct ps4_if ifs[MAX_IFS];
    struct if_nameindex *list;
    char *names;
    int n = list_ifs( ifs, MAX_IFS ), i;
    static int told;

    if (!(list = malloc( (n + 1) * sizeof(*list) + n * 16 )))
    {
        errno = ENOBUFS;
        return NULL;
    }
    names = (char *)(list + n + 1);
    for (i = 0; i < n; i++)
    {
        list[i].if_index = ifs[i].index;
        list[i].if_name = strcpy( names + i * 16, ifs[i].name );
    }
    list[n].if_index = 0;
    list[n].if_name = NULL;
    if (!told++)
    {
        fprintf( stderr, "wineps4: network interfaces:" );
        for (i = 0; i < n; i++) fprintf( stderr, " %s (%u, type %#x)", ifs[i].name, ifs[i].index, ifs[i].type );
        fprintf( stderr, "%s\n", n ? "" : " none" );
    }
    return list;
}

void if_freenameindex( struct if_nameindex *list )
{
    free( list );
}

unsigned int if_nametoindex( const char *name )
{
    struct ps4_if ifs[MAX_IFS];
    int n = list_ifs( ifs, MAX_IFS ), i;

    for (i = 0; i < n; i++)
        if (!strcmp( ifs[i].name, name )) return ifs[i].index;
    errno = ENXIO;
    return 0;
}

char *if_indextoname( unsigned int index, char *name )
{
    struct ps4_if ifs[MAX_IFS];
    int n = list_ifs( ifs, MAX_IFS ), i;

    for (i = 0; i < n; i++)
        if (ifs[i].index == index) return strcpy( name, ifs[i].name );
    errno = ENXIO;
    return NULL;
}

struct ifaddrs_node
{
    struct ifaddrs ifa;
    char name[16];
    struct sockaddr_storage addr, mask, brd;
};

/* (the C library's sockaddr_in6 and sockaddr_storage are Linux's shape, its sockaddr FreeBSD's: the
 * length and the family are set through the latter) */
static void set_family( void *sa, int len, int family )
{
    ((struct sockaddr *)sa)->sa_len = len;
    ((struct sockaddr *)sa)->sa_family = family;
}

/* an IPv6 address's mask: 128 bits for the loopback, else 64 (the kernel's own needs an ioctl whose
 * size is FreeBSD's in6_ifreq) */
static void ipv6_mask( const struct sockaddr_in6 *addr, struct sockaddr_in6 *mask )
{
    static const unsigned char loopback[16] = { [15] = 1 };
    const int bits = !memcmp( &addr->sin6_addr, loopback, 16 ) ? 128 : 64;

    memset( mask, 0, sizeof(*mask) );
    set_family( mask, sizeof(*mask), AF_INET6 );
    memset( &mask->sin6_addr, 0xff, bits / 8 );
}

int getifaddrs( struct ifaddrs **result )
{
    char *buf = malloc( IFCONF_SIZE );
    struct ifaddrs *head = NULL, **tail = &head;
    int sock = socket( AF_INET, SOCK_DGRAM, 0 ), len = -1, pos;

    *result = NULL;
    if (sock >= 0 && buf) len = read_ifconf( sock, buf, IFCONF_SIZE );
    if (len < 0)
    {
        if (sock >= 0) close( sock );
        free( buf );
        return -1;
    }
    for (pos = 0; pos + (int)sizeof(struct fbsd_ifreq) <= len; )
    {
        const struct fbsd_ifreq *req = (const struct fbsd_ifreq *)(buf + pos);
        const struct sockaddr *sa = &req->u.addr;
        struct fbsd_ifreq q;
        struct ifaddrs_node *node;
        int flags = 0;

        pos += entry_size( sa );
        if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;
        if (sa->sa_len > sizeof(node->addr) || !(node = calloc( 1, sizeof(*node) ))) continue;
        memcpy( node->name, req->name, 15 );
        memcpy( &node->addr, sa, sa->sa_len );
        node->ifa.ifa_name = node->name;
        node->ifa.ifa_addr = (struct sockaddr *)&node->addr;
        node->ifa.ifa_netmask = (struct sockaddr *)&node->mask;

        memset( &q, 0, sizeof(q) );
        memcpy( q.name, req->name, 16 );
        if (!ps4_ioctl( sock, FBSD_SIOCGIFFLAGS, &q ))
        {
            flags = (unsigned short)q.u.flags[0] | ((unsigned short)q.u.flags[1] << 16);
            if (flags & FBSD_IFF_MULTICAST) flags = (flags & ~FBSD_IFF_MULTICAST) | LIBC_IFF_MULTICAST;
        }
        node->ifa.ifa_flags = flags;
        if (sa->sa_family == AF_INET)
        {
            memset( &q.u, 0, sizeof(q.u) );
            if (!ps4_ioctl( sock, FBSD_SIOCGIFNETMASK, &q ))
                memcpy( &node->mask, &q.u.addr, sizeof(struct sockaddr_in) );
            set_family( &node->mask, sizeof(struct sockaddr_in), AF_INET );
            memset( &q.u, 0, sizeof(q.u) );
            if ((flags & FBSD_IFF_BROADCAST) && !ps4_ioctl( sock, FBSD_SIOCGIFBRDADDR, &q ))
            {
                memcpy( &node->brd, &q.u.addr, sizeof(struct sockaddr_in) );
                node->ifa.ifa_broadaddr = (struct sockaddr *)&node->brd;
            }
        }
        else ipv6_mask( (const struct sockaddr_in6 *)sa, (struct sockaddr_in6 *)&node->mask );
        *tail = &node->ifa;
        tail = &node->ifa.ifa_next;
    }
    close( sock );
    free( buf );
    *result = head;
    return 0;
}

void freeifaddrs( struct ifaddrs *list )
{
    while (list)
    {
        struct ifaddrs *next = list->ifa_next;
        free( list );  /* (each is the start of its ifaddrs_node) */
        list = next;
    }
}

/* --- Routes and the console's network settings --------------------------------------------------
 * For nsiproxy's IPv4 route table (ip.c) and dnsapi's server list (dlls/dnsapi/ps4.c): without
 * either, GetAdaptersAddresses failed as a whole (v00.52: "failed: 1", the route table's "not
 * implemented"). The routes come from FreeBSD's routing sysctl (NET_RT_DUMP, made with the raw
 * system call: the C library's headers have neither its numbers nor its structures), else from the
 * interfaces' own networks; the gateway and the DNS servers from the PS4's network settings
 * (sceNetCtl). */

int32_t sceNetCtlInit( void );
int32_t sceNetCtlGetInfo( int32_t code, void *info );
extern int ps4_kit_load_module( uint32_t id, const char *name );
extern int ps4_outside_process;  /* port/ps4_main.c */

#define SYSMODULE_NET    0x8000001c
#define SYSMODULE_NETCTL 0x80000009

#define NETCTL_INFO_DEFAULT_ROUTE 16
#define NETCTL_INFO_PRIMARY_DNS   17
#define NETCTL_INFO_SECONDARY_DNS 18

struct ps4_route  /* (dlls/nsiproxy.sys/ip.c has it too) */
{
    unsigned int prefix, next_hop;  /* network byte order */
    unsigned int prefix_len, if_index, gateway;
};

static struct
{
    unsigned int gateway, dns[2];  /* network byte order; 0: none */
} net_info;
static pthread_once_t net_info_once = PTHREAD_ONCE_INIT;

static unsigned int netctl_address( int code )
{
    union { char text[260]; unsigned int align; } info;  /* (SceNetCtlInfo: 256 bytes at most) */
    struct in_addr addr;

    memset( &info, 0, sizeof(info) );
    if (sceNetCtlGetInfo( code, &info )) return 0;
    info.text[15] = 0;
    return inet_pton( AF_INET, info.text, &addr ) == 1 ? addr.s_addr : 0;
}

static const char *dotted( unsigned int addr, char *buf )
{
    struct in_addr a;

    a.s_addr = addr;
    return addr && inet_ntop( AF_INET, &a, buf, 16 ) ? buf : "none";
}

static void read_net_info( void )
{
    char a[16], b[16], c[16];
    int ret;

    /* (not in a process outside the 7, nsiproxy's winedevice.exe among them: no system module has
     * been loaded outside the app yet, and the drives go with it if that fails) */
    if (ps4_outside_process) return;
    /* (a system library whose module isn't loaded ends the app at its first call, without a word:
     * ps4-kit's ps4_modules.cpp) */
    ps4_kit_load_module( SYSMODULE_NET, "Net" );
    if ((ret = ps4_kit_load_module( SYSMODULE_NETCTL, "NetCtl" )))
    {
        fprintf( stderr, "wineps4: network settings: the PS4's NetCtl module did not load (%#x)\n", ret );
        return;
    }
    ret = sceNetCtlInit();  /* (an error when it was already: then it is) */
    net_info.gateway = netctl_address( NETCTL_INFO_DEFAULT_ROUTE );
    net_info.dns[0] = netctl_address( NETCTL_INFO_PRIMARY_DNS );
    net_info.dns[1] = netctl_address( NETCTL_INFO_SECONDARY_DNS );
    fprintf( stderr, "wineps4: network settings: gateway %s, DNS %s %s (sceNetCtlInit %#x)\n",
             dotted( net_info.gateway, a ), dotted( net_info.dns[0], b ), dotted( net_info.dns[1], c ), ret );
}

/* the DNS servers (network byte order): how many; the gateway when the PS4 names none */
int ps4_dns_servers( unsigned int *servers, int max )
{
    int n = 0, i;

    pthread_once( &net_info_once, read_net_info );
    for (i = 0; i < 2 && n < max; i++)
        if (net_info.dns[i]) servers[n++] = net_info.dns[i];
    if (!n && max && net_info.gateway) servers[n++] = net_info.gateway;
    return n;
}

static int raw_sysctl( int *mib, unsigned int len, void *old, size_t *oldlen )
{
    register long r10 __asm__("r10") = (long)oldlen;
    register long r8 __asm__("r8") = 0;  /* (nothing new) */
    register long r9 __asm__("r9") = 0;
    long rax = 202 /* SYS___sysctl */, rdx = (long)old;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "+a" (rax), "=qm" (failed), "+d" (rdx), "+r" (r10), "+r" (r8), "+r" (r9)
                      : "D" ((long)mib), "S" ((long)len)
                      : "rcx", "r11", "memory", "cc" );
    if (failed)
    {
        errno = (int)rax;
        return -1;
    }
    return 0;
}

static unsigned int mask_bits( unsigned int mask )
{
    unsigned int bits = 0, host = ntohl( mask );

    while (bits < 32 && (host & (0x80000000u >> bits))) bits++;
    return bits;
}

struct fbsd_rt_msghdr  /* FreeBSD's struct rt_msghdr on amd64 (152 bytes) */
{
    unsigned short msglen;
    unsigned char version, type;
    unsigned short index;
    int flags, addrs, pid, seq, err, fmask;
    unsigned long inits;
    unsigned long rmx[14];
};

#define FBSD_RTM_GET       4
#define FBSD_RTF_GATEWAY   0x2
#define FBSD_RTF_HOST      0x4
#define FBSD_RTF_MULTICAST 0x800000
#define FBSD_RTA_DST       0x1
#define FBSD_RTA_GATEWAY   0x2
#define FBSD_RTA_NETMASK   0x4

/* the kernel's IPv4 routes: how many, or -1 (the sandbox's no) */
static int routes_from_kernel( struct ps4_route *routes, int max )
{
    int mib[6] = { 4 /* CTL_NET */, 17 /* PF_ROUTE */, 0, AF_INET, 1 /* NET_RT_DUMP */, 0 }, n = 0;
    size_t size = 0;
    char *buf, *p, *end;

    if (raw_sysctl( mib, 6, NULL, &size ) || !size) return -1;
    size += size / 2 + 4096;  /* (more may come meanwhile) */
    if (!(buf = malloc( size ))) return -1;
    if (raw_sysctl( mib, 6, buf, &size ))
    {
        free( buf );
        return -1;
    }
    for (p = buf, end = buf + size; p + sizeof(struct fbsd_rt_msghdr) <= end && n < max; )
    {
        const struct fbsd_rt_msghdr *rtm = (const struct fbsd_rt_msghdr *)p;
        const char *sa_ptr = p + sizeof(*rtm), *msg_end = p + rtm->msglen;
        struct ps4_route r;
        int bit;

        if (rtm->msglen < sizeof(*rtm) || msg_end > end) break;
        p = (char *)msg_end;
        if (rtm->type != FBSD_RTM_GET) continue;
        if ((rtm->flags & FBSD_RTF_GATEWAY) && (rtm->flags & FBSD_RTF_MULTICAST)) continue;
        memset( &r, 0, sizeof(r) );
        r.if_index = rtm->index;
        r.gateway = !!(rtm->flags & FBSD_RTF_GATEWAY);
        r.prefix_len = (rtm->flags & FBSD_RTF_HOST) ? 32 : 0;  /* (no netmask: a host route, or the default) */
        /* the addresses, each padded to a long (FreeBSD's SA_SIZE) */
        for (bit = 1; bit <= 0x80 && sa_ptr < msg_end; bit <<= 1)
        {
            const struct sockaddr *sa = (const struct sockaddr *)sa_ptr;
            struct sockaddr_in sin;

            if (!(rtm->addrs & bit)) continue;
            memset( &sin, 0, sizeof(sin) );
            if (sa->sa_len) memcpy( &sin, sa, sa->sa_len < sizeof(sin) ? sa->sa_len : sizeof(sin) );
            if (bit == FBSD_RTA_DST && sa->sa_family == AF_INET) r.prefix = sin.sin_addr.s_addr;
            else if (bit == FBSD_RTA_GATEWAY && sa->sa_family == AF_INET) r.next_hop = sin.sin_addr.s_addr;
            else if (bit == FBSD_RTA_NETMASK) r.prefix_len = mask_bits( sin.sin_addr.s_addr );
            sa_ptr += sa->sa_len ? (sa->sa_len + sizeof(long) - 1) & ~(sizeof(long) - 1) : sizeof(long);
        }
        routes[n++] = r;
    }
    free( buf );
    return n;
}

/* the interfaces' own networks, and the default route by the PS4's gateway */
static int routes_from_addresses( struct ps4_route *routes, int max )
{
    struct ifaddrs *list, *ifa;
    int n = 0, gateway_if = 0;

    if (getifaddrs( &list )) return 0;
    for (ifa = list; ifa && n < max; ifa = ifa->ifa_next)
    {
        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        const struct sockaddr_in *mask = (const struct sockaddr_in *)ifa->ifa_netmask;
        const unsigned int m = mask ? mask->sin_addr.s_addr : ~0u;
        unsigned int index;

        if (!addr || addr->sin_family != AF_INET || !(index = if_nametoindex( ifa->ifa_name ))) continue;
        memset( &routes[n], 0, sizeof(routes[n]) );
        routes[n].if_index = index;
        routes[n].prefix_len = mask_bits( m );
        routes[n].prefix = addr->sin_addr.s_addr & m;
        n++;
        if (net_info.gateway && (net_info.gateway & m) == (addr->sin_addr.s_addr & m)) gateway_if = index;
    }
    freeifaddrs( list );
    if (gateway_if && n < max)
    {
        memset( &routes[n], 0, sizeof(routes[n]) );
        routes[n].if_index = gateway_if;
        routes[n].next_hop = net_info.gateway;
        routes[n].gateway = 1;
        n++;
    }
    return n;
}

/* the IPv4 routes (nsiproxy's ip.c): how many */
int ps4_ipv4_routes( struct ps4_route *routes, int max )
{
    static int told;
    int n = routes_from_kernel( routes, max );

    if (n < 0)
    {
        if (!told++)
            fprintf( stderr, "wineps4: no routing table from the kernel (%s): routes from the interfaces\n",
                     strerror( errno ) );
        pthread_once( &net_info_once, read_net_info );
        n = routes_from_addresses( routes, max );
    }
    return n;
}
