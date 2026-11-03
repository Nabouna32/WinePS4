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
