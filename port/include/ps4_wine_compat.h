/* WinePS4: included first in every Unix-side file of Wine (ntdll's Unix half, wineserver).
 *
 * The OpenOrbis C library mixes two worlds: its structures and most constants are FreeBSD's (the
 * PS4 kernel's), but bits/signal.h, time.h and a few others still carry musl's Linux numbers.
 * Everything Wine passes to the kernel must be FreeBSD's, so those are corrected here, before any
 * Wine header looks at them. */
#ifndef PS4_WINE_COMPAT_H
#define PS4_WINE_COMPAT_H

#ifndef __ASSEMBLER__

/* musl's stack_t (ss_flags before ss_size) is not the kernel's: keep musl's out of the way. */
#define sigaltstack __musl_sigaltstack
#include <signal.h>
#undef sigaltstack
#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* --- Signal numbers (FreeBSD) ------------------------------------------------------------------ */
#undef SIGBUS
#undef SIGUSR1
#undef SIGUSR2
#undef SIGCHLD
#undef SIGCONT
#undef SIGSTOP
#undef SIGTSTP
#undef SIGURG
#undef SIGIO
#undef SIGPOLL
#undef SIGSYS
#undef SIGPWR
#undef SIGSTKFLT
#undef SIGUNUSED
#undef SIGRTMIN
#undef SIGRTMAX
#define SIGEMT    7
#define SIGBUS    10
#define SIGSYS    12
#define SIGURG    16
#define SIGSTOP   17
#define SIGTSTP   18
#define SIGCONT   19
#define SIGCHLD   20
#define SIGIO     23
#define SIGINFO   29
#define SIGUSR1   30
#define SIGUSR2   31

#undef SA_NOCLDSTOP
#undef SA_NOCLDWAIT
#undef SA_SIGINFO
#undef SA_ONSTACK
#undef SA_RESTART
#undef SA_NODEFER
#undef SA_RESETHAND
#undef SA_RESTORER
#define SA_ONSTACK    0x0001
#define SA_RESTART    0x0002
#define SA_RESETHAND  0x0004
#define SA_NOCLDSTOP  0x0008
#define SA_NODEFER    0x0010
#define SA_NOCLDWAIT  0x0020
#define SA_SIGINFO    0x0040

#undef SIG_BLOCK
#undef SIG_UNBLOCK
#undef SIG_SETMASK
#define SIG_BLOCK     1
#define SIG_UNBLOCK   2
#define SIG_SETMASK   3

#undef SS_ONSTACK
#undef SS_DISABLE
#define SS_ONSTACK    0x0001
#define SS_DISABLE    0x0004
#undef MINSIGSTKSZ
#undef SIGSTKSZ
#define MINSIGSTKSZ   2048
#define SIGSTKSZ      (MINSIGSTKSZ + 32768)

#undef FPE_INTDIV
#undef FPE_INTOVF
#define FPE_INTOVF    1
#define FPE_INTDIV    2
#undef SI_USER
#undef SI_QUEUE
#undef SI_TIMER
#undef SI_KERNEL
#define SI_USER       0x10001
#define SI_QUEUE      0x10002
#define SI_TIMER      0x10003

/* the kernel's sigaltstack */
typedef struct ps4_stack
{
    void  *ss_sp;
    size_t ss_size;
    int    ss_flags;
} ps4_stack_t;
#define stack_t ps4_stack_t
int sigaltstack( const stack_t *ss, stack_t *old );

/* --- Signal context ------------------------------------------------------------------------------
 * FreeBSD amd64's mcontext, at offset 0x40 of the PS4's ucontext (ps4-kit's crash handler reads
 * the same offsets; checked on the console by WinePS4's probe). */
typedef struct ps4_mcontext
{
    long mc_onstack;
    long mc_rdi, mc_rsi, mc_rdx, mc_rcx, mc_r8, mc_r9, mc_rax, mc_rbx, mc_rbp;
    long mc_r10, mc_r11, mc_r12, mc_r13, mc_r14, mc_r15;
    unsigned int mc_trapno;
    unsigned short mc_fs, mc_gs;
    long mc_addr;
    unsigned int mc_flags;
    unsigned short mc_es, mc_ds;
    long mc_err, mc_rip, mc_cs, mc_rflags, mc_rsp, mc_ss;
    long mc_len, mc_fpformat, mc_ownedfp;
    long mc_fpstate[64] __attribute__((aligned(16)));
    long mc_fsbase, mc_gsbase;
    long mc_xfpustate, mc_xfpustate_len;
    long mc_spare[4];
} ps4_mcontext_t;

typedef struct ps4_ucontext
{
    sigset_t uc_sigmask;
    unsigned char __ps4_reserved[0x40 - sizeof(sigset_t)];
    ps4_mcontext_t uc_mcontext;
} ps4_ucontext_t;
_Static_assert( offsetof(ps4_ucontext_t, uc_mcontext) == 0x40, "PS4 mcontext offset" );
_Static_assert( offsetof(ps4_mcontext_t, mc_rip) == 20 * 8, "PS4 mcontext rip" );
_Static_assert( offsetof(ps4_mcontext_t, mc_fpstate) == 28 * 8, "PS4 mcontext fpstate" );

#define ucontext_t ps4_ucontext_t
#define mcontext_t ps4_mcontext_t

/* --- Clocks: ids as the kernel numbers them, except the monotonic ones, which go to ps4-kit's fast
 * CLOCK_MONOTONIC (its wrap recognises musl's number 1). */
#undef CLOCK_MONOTONIC_RAW
#undef CLOCK_MONOTONIC_COARSE
#undef CLOCK_REALTIME_COARSE
#undef CLOCK_BOOTTIME
#undef CLOCK_PROCESS_CPUTIME_ID
#undef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_MONOTONIC_RAW       CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_COARSE    CLOCK_MONOTONIC
#define CLOCK_BOOTTIME            CLOCK_MONOTONIC
#define CLOCK_REALTIME_COARSE     CLOCK_REALTIME
#define CLOCK_THREAD_CPUTIME_ID   14
#define CLOCK_PROCESS_CPUTIME_ID  15

/* --- FreeBSD calls the PS4 has under other names, or not at all (port/ps4_wine_port.c) ---------- */
int amd64_get_gsbase( void **base );
int amd64_set_gsbase( void *base );
int amd64_get_fsbase( void **base );
int amd64_set_fsbase( void *base );
int pthread_getthreadid_np( void );
/* Signals to a thread by its pthread_getthreadid_np() id (wineserver's suspend and APC signals). */
int ps4_thread_kill( int tid, int sig );
/* ... of any process (thr_kill2 for the other program processes) */
int ps4_thread_kill2( int pid, int tid, int sig );
/* Records the calling thread for ps4_thread_kill (ntdll does it as each thread starts). */
void ps4_thread_register( void );
void ps4_thread_unregister( void );
/* Built-in Unix libraries: dlopen/dlsym answer from a table instead of loading files. */
void *ps4_dlopen( const char *name, int flags );
void *ps4_dlsym( void *handle, const char *name );
int ps4_dlclose( void *handle );
char *ps4_dlerror( void );

/* --- Sockets ---------------------------------------------------------------------------------------
 * OpenOrbis keeps musl's (Linux) msghdr and cmsghdr layouts and MSG_ numbers; the kernel's are
 * FreeBSD's. wineserver passes every handle's descriptor through these. Its sockaddr_un,
 * sockaddr_in6 and sockaddr_storage are musl's too: no length byte before the 1-byte family, so the
 * kernel took AF_UNIX for a length of 1 and the path's '/' for the family (v00.20: wineserver's
 * bind failed, "No such file or directory"). Its sockaddr and sockaddr_in are FreeBSD's already. */
#define msghdr __musl_msghdr
#define cmsghdr __musl_cmsghdr
#define sockaddr_un __musl_sockaddr_un
#define sockaddr_in6 __musl_sockaddr_in6
#define sockaddr_storage __musl_sockaddr_storage
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#undef msghdr
#undef cmsghdr
#undef sockaddr_un
#undef sockaddr_in6
#undef sockaddr_storage
struct sockaddr_un
{
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};
struct sockaddr_in6
{
    unsigned char sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};
struct sockaddr_storage
{
    unsigned char ss_len;
    sa_family_t ss_family;
    char __ss_pad1[6];
    long long __ss_align;
    char __ss_pad2[112];
};
_Static_assert( sizeof(struct sockaddr_in6) == 28, "FreeBSD's sockaddr_in6" );
_Static_assert( sizeof(struct sockaddr_storage) == 128, "FreeBSD's sockaddr_storage" );
#define HAVE_STRUCT_SOCKADDR_UN_SUN_LEN 1
struct msghdr
{
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    void         *msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};
struct cmsghdr
{
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
};
#define PS4_CMSG_ALIGN(n) (((size_t)(n) + 7) & ~(size_t)7)
#undef CMSG_ALIGN
#undef CMSG_DATA
#undef CMSG_LEN
#undef CMSG_SPACE
#undef CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#define CMSG_ALIGN(n)     PS4_CMSG_ALIGN(n)
#define CMSG_DATA(c)      ((unsigned char *)(c) + PS4_CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(n)       (PS4_CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n)     (PS4_CMSG_ALIGN(sizeof(struct cmsghdr)) + PS4_CMSG_ALIGN(n))
#define CMSG_FIRSTHDR(m)  ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(m, c) \
    (!(c) ? CMSG_FIRSTHDR(m) : \
     ((char *)(c) + PS4_CMSG_ALIGN((c)->cmsg_len) + PS4_CMSG_ALIGN(sizeof(struct cmsghdr)) > \
      (char *)(m)->msg_control + (m)->msg_controllen) ? (struct cmsghdr *)0 : \
     (struct cmsghdr *)((char *)(c) + PS4_CMSG_ALIGN((c)->cmsg_len)))
#undef MSG_OOB
#undef MSG_PEEK
#undef MSG_DONTROUTE
#undef MSG_EOR
#undef MSG_TRUNC
#undef MSG_CTRUNC
#undef MSG_WAITALL
#undef MSG_DONTWAIT
#undef MSG_EOF
#undef MSG_NOSIGNAL
#undef MSG_CMSG_CLOEXEC
#undef MSG_CONFIRM
#undef MSG_PROXY
#undef MSG_FIN
#undef MSG_SYN
#undef MSG_RST
#undef MSG_ERRQUEUE
#undef MSG_MORE
#undef MSG_WAITFORONE
#undef MSG_FASTOPEN
#undef MSG_ZEROCOPY
#define MSG_OOB           0x00001
#define MSG_PEEK          0x00002
#define MSG_DONTROUTE     0x00004
#define MSG_EOR           0x00008
#define MSG_TRUNC         0x00010
#define MSG_CTRUNC        0x00020
#define MSG_WAITALL       0x00040
#define MSG_DONTWAIT      0x00080
#define MSG_EOF           0x00100
#define MSG_NOSIGNAL      0x20000
#define MSG_CMSG_CLOEXEC  0x40000
/* Linux-only options OpenOrbis still defines */
#undef SO_ATTACH_FILTER
#undef SO_DETACH_FILTER
#undef SO_BINDTODEVICE
#undef SO_BINDTOIFINDEX
#undef SO_PASSSEC
#undef SO_MARK
#undef SO_PEERNAME
#undef SCM_CREDENTIALS

/* IP, IPv6 and TCP options: OpenOrbis' netinet headers have Linux's numbers (its SOL_SOCKET ones are
 * FreeBSD's). Steam's IP_TOS (Linux 1) went to the kernel as IP_OPTIONS, "Unable to set default socket
 * options, error 10022", and every ping of its login servers failed (v00.54). FreeBSD 9's numbers
 * here; the options it lacks are undefined, so Wine takes its other ways: IP_RECVDSTADDR for
 * IP_PKTINFO, IP_DONTFRAG and IPV6_DONTFRAG for the MTU discovery options. The source-specific
 * multicast ones go too: musl's struct ip_mreq_source has its fields in another order. */
#include <netinet/tcp.h>
#undef IP_OPTIONS
#undef IP_HDRINCL
#undef IP_TOS
#undef IP_TTL
#undef IP_RECVOPTS
#undef IP_RECVRETOPTS
#undef IP_RETOPTS
#undef IP_MULTICAST_IF
#undef IP_MULTICAST_TTL
#undef IP_MULTICAST_LOOP
#undef IP_ADD_MEMBERSHIP
#undef IP_DROP_MEMBERSHIP
#undef IP_RECVTTL
#undef IP_MINTTL
#undef IP_PKTINFO
#undef IP_PKTOPTIONS
#undef IP_PMTUDISC
#undef IP_MTU_DISCOVER
#undef IP_PMTUDISC_DONT
#undef IP_PMTUDISC_WANT
#undef IP_PMTUDISC_DO
#undef IP_PMTUDISC_PROBE
#undef IP_PMTUDISC_INTERFACE
#undef IP_PMTUDISC_OMIT
#undef IP_RECVERR
#undef IP_RECVTOS
#undef IP_MTU
#undef IP_FREEBIND
#undef IP_IPSEC_POLICY
#undef IP_XFRM_POLICY
#undef IP_PASSSEC
#undef IP_TRANSPARENT
#undef IP_ORIGDSTADDR
#undef IP_RECVORIGDSTADDR
#undef IP_NODEFRAG
#undef IP_CHECKSUM
#undef IP_BIND_ADDRESS_NO_PORT
#undef IP_RECVFRAGSIZE
#undef IP_ROUTER_ALERT
#undef IP_MULTICAST_ALL
#undef IP_UNICAST_IF
#undef IP_MSFILTER
#undef IP_ADD_SOURCE_MEMBERSHIP
#undef IP_DROP_SOURCE_MEMBERSHIP
#undef IP_BLOCK_SOURCE
#undef IP_UNBLOCK_SOURCE
#undef MCAST_JOIN_GROUP
#undef MCAST_LEAVE_GROUP
#undef MCAST_JOIN_SOURCE_GROUP
#undef MCAST_LEAVE_SOURCE_GROUP
#undef MCAST_BLOCK_SOURCE
#undef MCAST_UNBLOCK_SOURCE
#undef MCAST_MSFILTER
#define IP_OPTIONS          1
#define IP_HDRINCL          2
#define IP_TOS              3
#define IP_TTL              4
#define IP_RECVOPTS         5
#define IP_RECVRETOPTS      6
#define IP_RECVDSTADDR      7
#define IP_RETOPTS          8
#define IP_MULTICAST_IF     9
#define IP_MULTICAST_TTL    10
#define IP_MULTICAST_LOOP   11
#define IP_ADD_MEMBERSHIP   12
#define IP_DROP_MEMBERSHIP  13
#define IP_RECVIF           20
#define IP_RECVTTL          65
#define IP_MINTTL           66
#define IP_DONTFRAG         67
#define MCAST_JOIN_GROUP    80
#define MCAST_LEAVE_GROUP   81

#undef IPV6_ADDRFORM
#undef IPV6_2292PKTINFO
#undef IPV6_2292HOPOPTS
#undef IPV6_2292DSTOPTS
#undef IPV6_2292RTHDR
#undef IPV6_2292PKTOPTIONS
#undef IPV6_2292HOPLIMIT
#undef IPV6_CHECKSUM
#undef IPV6_NEXTHOP
#undef IPV6_AUTHHDR
#undef IPV6_UNICAST_HOPS
#undef IPV6_MULTICAST_IF
#undef IPV6_MULTICAST_HOPS
#undef IPV6_MULTICAST_LOOP
#undef IPV6_JOIN_GROUP
#undef IPV6_LEAVE_GROUP
#undef IPV6_ADD_MEMBERSHIP
#undef IPV6_DROP_MEMBERSHIP
#undef IPV6_ROUTER_ALERT
#undef IPV6_ROUTER_ALERT_ISOLATE
#undef IPV6_MTU_DISCOVER
#undef IPV6_PMTUDISC_DONT
#undef IPV6_PMTUDISC_WANT
#undef IPV6_PMTUDISC_DO
#undef IPV6_PMTUDISC_PROBE
#undef IPV6_PMTUDISC_INTERFACE
#undef IPV6_PMTUDISC_OMIT
#undef IPV6_MTU
#undef IPV6_RECVERR
#undef IPV6_V6ONLY
#undef IPV6_JOIN_ANYCAST
#undef IPV6_LEAVE_ANYCAST
#undef IPV6_MULTICAST_ALL
#undef IPV6_IPSEC_POLICY
#undef IPV6_XFRM_POLICY
#undef IPV6_HDRINCL
#undef IPV6_RECVPKTINFO
#undef IPV6_PKTINFO
#undef IPV6_RECVHOPLIMIT
#undef IPV6_HOPLIMIT
#undef IPV6_RECVHOPOPTS
#undef IPV6_HOPOPTS
#undef IPV6_RTHDRDSTOPTS
#undef IPV6_RECVRTHDR
#undef IPV6_RTHDR
#undef IPV6_RECVDSTOPTS
#undef IPV6_DSTOPTS
#undef IPV6_RECVPATHMTU
#undef IPV6_PATHMTU
#undef IPV6_DONTFRAG
#undef IPV6_RECVTCLASS
#undef IPV6_TCLASS
#undef IPV6_AUTOFLOWLABEL
#undef IPV6_ADDR_PREFERENCES
#undef IPV6_MINHOPCOUNT
#undef IPV6_ORIGDSTADDR
#undef IPV6_RECVORIGDSTADDR
#undef IPV6_TRANSPARENT
#undef IPV6_UNICAST_IF
#undef IPV6_RECVFRAGSIZE
#undef IPV6_FREEBIND
#undef IPV6_RXHOPOPTS
#undef IPV6_RXDSTOPTS
#define IPV6_UNICAST_HOPS    4
#define IPV6_MULTICAST_IF    9
#define IPV6_MULTICAST_HOPS  10
#define IPV6_MULTICAST_LOOP  11
#define IPV6_JOIN_GROUP      12
#define IPV6_LEAVE_GROUP     13
#define IPV6_ADD_MEMBERSHIP  IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#define IPV6_CHECKSUM        26
#define IPV6_V6ONLY          27
#define IPV6_RECVPKTINFO     36
#define IPV6_RECVHOPLIMIT    37
#define IPV6_PKTINFO         46
#define IPV6_HOPLIMIT        47
#define IPV6_RECVTCLASS      57
#define IPV6_TCLASS          61
#define IPV6_DONTFRAG        62

#undef TCP_CORK
#undef TCP_KEEPIDLE
#undef TCP_KEEPINTVL
#undef TCP_KEEPCNT
#undef TCP_SYNCNT
#undef TCP_LINGER2
#undef TCP_DEFER_ACCEPT
#undef TCP_WINDOW_CLAMP
#undef TCP_INFO
#undef TCP_QUICKACK
#undef TCP_CONGESTION
#undef TCP_MD5SIG
#undef TCP_THIN_LINEAR_TIMEOUTS
#undef TCP_THIN_DUPACK
#undef TCP_USER_TIMEOUT
#undef TCP_REPAIR
#undef TCP_REPAIR_QUEUE
#undef TCP_QUEUE_SEQ
#undef TCP_REPAIR_OPTIONS
#undef TCP_FASTOPEN
#undef TCP_TIMESTAMP
#undef TCP_NOTSENT_LOWAT
#undef TCP_CC_INFO
#undef TCP_SAVE_SYN
#undef TCP_SAVED_SYN
#undef TCP_REPAIR_WINDOW
#undef TCP_FASTOPEN_CONNECT
#undef TCP_ULP
#undef TCP_MD5SIG_EXT
#undef TCP_FASTOPEN_KEY
#undef TCP_FASTOPEN_NO_COOKIE
#undef TCP_ZEROCOPY_RECEIVE
#undef TCP_INQ
#undef TCP_TX_DELAY
#undef TCP_CM_INQ
#define TCP_NOPUSH    4
#define TCP_NOOPT     8
#define TCP_INFO      32   /* (its struct starts with the state as Linux's does: wineserver's TCP table) */
#define TCP_KEEPINIT  128
#define TCP_KEEPIDLE  256
#define TCP_KEEPINTVL 512
#define TCP_KEEPCNT   1024

/* --- Memory ------------------------------------------------------------------------------------------
 * MAP_NORESERVE (0x4000) and MAP_FIXED_NOREPLACE (0x100000) are Linux numbers in OpenOrbis' header;
 * the PS4 kernel reads those bits as something else. Wine works without either (an address hint,
 * then a check of where the mapping landed). Wine's mappings go to port/ps4_wine_vm.c, not to
 * ps4-kit's wrap: there inaccessible mappings only reserve address space and pages get direct memory
 * when they become accessible (every PS4 mapping takes memory, accessible or not). */
#include <sys/mman.h>
#undef MAP_NORESERVE
#define MAP_NORESERVE 0
#undef MAP_FIXED_NOREPLACE
void *ps4_vm_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset );
int ps4_vm_munmap( void *addr, size_t len );
int ps4_vm_mprotect( void *addr, size_t len, int prot );
int ps4_vm_madvise( void *addr, size_t len, int advice );
void *ps4_vm_address_limit( void );
#define PS4_HOST_PAGE_SIZE 0x4000  /* the PS4's page: where Wine's 4 KiB pages share one */
#define mmap ps4_vm_mmap
#define munmap ps4_vm_munmap
#define mprotect ps4_vm_mprotect
#define madvise ps4_vm_madvise

/* --- Folders by descriptor ---------------------------------------------------------------------------
 * The PS4 kernel takes only absolute paths: calls relative to a folder's descriptor go to
 * port/ps4_wine_dirs.c, which remembers the path of every folder opened by name. */
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
int ps4_wine_open( const char *path, int flags, ... );
int ps4_wine_openat( int dirfd, const char *name, int flags, ... );
int ps4_wine_fstatat( int dirfd, const char *name, struct stat *st, int flags );
int ps4_wine_fchdir( int fd );
DIR *ps4_wine_fdopendir( int fd );
#define open(...) ps4_wine_open( __VA_ARGS__ )
#define openat ps4_wine_openat
#define fstatat ps4_wine_fstatat
#define fchdir ps4_wine_fchdir
#define fdopendir ps4_wine_fdopendir

/* --- File locks --------------------------------------------------------------------------------------
 * OpenOrbis' struct flock and lock types are musl's (l_type first; F_RDLCK 0, F_WRLCK 1), the kernel's
 * are FreeBSD's (l_start first; F_RDLCK 1, F_WRLCK 3): every lock wineserver set failed with EINVAL,
 * and with it LockFile (SQLite's databases: Steam's web view, Chrome). fcntl hands the kernel its own
 * (port/ps4_wine_dirs.c). */
#undef F_RDLCK
#undef F_WRLCK
#undef F_UNLCK
#define F_RDLCK 1
#define F_UNLCK 2
#define F_WRLCK 3
#undef F_DUPFD_CLOEXEC  /* (Linux's 1030: the PS4's FreeBSD 9 has none) */
int ps4_wine_fcntl( int fd, int cmd, ... );
#define fcntl ps4_wine_fcntl

/* --- The end of the program ---------------------------------------------------------------------------
 * The desktop (shell/) shares the process: Wine's exit tells it the program ended and leaves the
 * calling thread waiting, instead of closing the app (port/ps4_wine_port.c). */
#include <stdlib.h>
#include <unistd.h>
void ps4_wine_exit( int status ) __attribute__((noreturn));
#define exit(status) ps4_wine_exit( status )
#define _exit(status) ps4_wine_exit( status )

/* --- ioctls and calls the PS4 kernel has with FreeBSD numbers, or does not have --------------------- */
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <errno.h>
#undef SIOCATMARK
#undef TIOCGWINSZ
#define SIOCATMARK  0x40047307  /* _IOR('s', 7, int) */
#define TIOCGWINSZ  0x40087468  /* _IOR('t', 104, struct winsize) */
/* OpenOrbis' struct statfs is Linux's; the kernel's is FreeBSD's (much larger). Not used: Wine then
 * reports NTFS and gets sizes from fstatvfs (ps4-kit). */
static inline int ps4_no_fstatfs( int fd, struct statfs *buf ) { (void)fd; (void)buf; errno = ENOSYS; return -1; }
#define fstatfs(fd, buf) ps4_no_fstatfs( fd, buf )
#define statfs(path, buf) ps4_no_fstatfs( -1, buf )

/* --- Resource limits (FreeBSD's numbers) ---------------------------------------------------------
 * musl's are Linux's: ntdll and wineserver raised "RLIMIT_NOFILE" (7, the kernel's RLIMIT_NPROC) to
 * its most at every start and left the open files at the PS4's default (v00.47: Steam got "Too many
 * open files" a second in: its free disk space, wininet.dll, its sockets all failed). */
#include <sys/resource.h>
#undef RLIMIT_MEMLOCK
#undef RLIMIT_NPROC
#undef RLIMIT_NOFILE
#undef RLIMIT_AS
#undef RLIMIT_LOCKS
#undef RLIMIT_SIGPENDING
#undef RLIMIT_MSGQUEUE
#undef RLIMIT_NICE
#undef RLIMIT_RTPRIO
#undef RLIMIT_RTTIME
#define RLIMIT_MEMLOCK 6
#define RLIMIT_NPROC   7
#define RLIMIT_NOFILE  8
#define RLIMIT_AS      10  /* RLIMIT_VMEM */

/* --- The console's name (port/ps4_net.c) ---------------------------------------------------------
 * The C library's gethostname is a stub (ENOSYS: Windows had no computer name), and a lookup of the
 * console's own name went to DNS through the PS4's resolver (ps4-kit): wineboot looks it up at every
 * start, and a name the router doesn't know takes up to 2 x 5 s there. */
struct addrinfo;
struct hostent;
int ps4_gethostname( char *name, size_t size );
int ps4_getaddrinfo( const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res );
struct hostent *ps4_gethostbyname( const char *name );
#define gethostname ps4_gethostname
#define getaddrinfo ps4_getaddrinfo
#define gethostbyname ps4_gethostbyname

/* The TEB of the calling thread, kept in ordinary TLS: inside a signal handler the PS4 does not give
 * the thread's GS base back, and Wine's handlers find the TEB through %gs. */
extern __thread void *ps4_thread_teb;

#define dlopen  ps4_dlopen
#define dlsym   ps4_dlsym
#define dlclose ps4_dlclose
#define dlerror ps4_dlerror

#endif /* __ASSEMBLER__ */
#endif /* PS4_WINE_COMPAT_H */
