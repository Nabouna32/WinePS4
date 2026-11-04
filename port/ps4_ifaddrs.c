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
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
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
