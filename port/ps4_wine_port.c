/* WinePS4: what Wine's Unix side expects from the system and the PS4 provides differently.
 *
 *  - wineserver as a thread of this process, reached through a socket pair (no fork/exec)
 *  - signals to a thread by its kernel thread id (wineserver's SuspendThread and APCs)
 *  - FreeBSD's amd64_{get,set}_{gs,fs}base on sysarch
 *  - dlopen/dlsym for the Unix libraries, which are linked into the eboot
 *  - the FreeBSD system calls libkernel does not export (symlink, readlink), made directly
 *  - stubs for what has no PS4 equivalent (fork, exec, ptrace)
 *  - a hang dump: where each Wine thread is when the program doesn't finish (hang.log) */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <machine/sysarch.h>

__thread void *ps4_thread_teb;

/* --- GS / FS base ------------------------------------------------------------------------------ */

int amd64_get_gsbase( void **base ) { return sysarch( AMD64_GET_GSBASE, base ); }
int amd64_set_gsbase( void *base ) { return sysarch( AMD64_SET_GSBASE, &base ); }
int amd64_get_fsbase( void **base ) { return sysarch( AMD64_GET_FSBASE, base ); }
int amd64_set_fsbase( void *base ) { return sysarch( AMD64_SET_FSBASE, &base ); }

/* --- Threads by kernel id ---------------------------------------------------------------------- */

/* Wine's threads (and the wineserver thread): their kernel id, pthread and TEB. A thread leaves the
 * table when it ends (the key's destructor): a signal to a pthread that has ended may end the app
 * (v00.16: the hang dump's, after the program was gone). */
#define MAX_THREADS 1024
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int tid; pthread_t thread; void *teb; } threads[MAX_THREADS];
static pthread_key_t thread_key;
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;

void ps4_thread_unregister( void );
static void thread_ended( void *arg ) { ps4_thread_unregister(); }
static void make_thread_key( void ) { pthread_key_create( &thread_key, thread_ended ); }

void ps4_thread_register( void )
{
    int tid = pthread_getthreadid_np(), i, free_slot = -1;

    pthread_once( &thread_key_once, make_thread_key );
    pthread_setspecific( thread_key, (void *)1 );
    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++)
    {
        if (threads[i].tid == tid) { free_slot = i; break; }
        if (!threads[i].tid && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0)
    {
        threads[free_slot].thread = pthread_self();
        __atomic_store_n( &threads[free_slot].teb, ps4_thread_teb, __ATOMIC_RELEASE );
        threads[free_slot].tid = tid;
    }
    pthread_mutex_unlock( &threads_lock );
    if (free_slot < 0) fprintf( stderr, "wineps4: thread table full, thread %d cannot be signalled\n", tid );
}

void ps4_thread_unregister( void )
{
    int tid = pthread_getthreadid_np(), i;

    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++)
        if (threads[i].tid == tid)
        {
            threads[i].tid = 0;
            __atomic_store_n( &threads[i].teb, NULL, __ATOMIC_RELEASE );
        }
    pthread_mutex_unlock( &threads_lock );
}

/* Whether a TEB is a Wine thread's: no lock, no thread data (it runs first in a signal handler, where
 * %fs may be the 32-bit TEB's) */
static int is_wine_teb( void *teb )
{
    int i;

    if (!teb) return 0;
    for (i = 0; i < MAX_THREADS; i++)
        if (__atomic_load_n( &threads[i].teb, __ATOMIC_ACQUIRE ) == teb) return 1;
    return 0;
}

int ps4_thread_kill( int tid, int sig )
{
    pthread_t thread;
    int i, found = 0, ret;

    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++)
    {
        if (threads[i].tid != tid) continue;
        thread = threads[i].thread;
        found = 1;
        break;
    }
    pthread_mutex_unlock( &threads_lock );
    if (!found)
    {
        errno = ESRCH;
        return -1;
    }
    if ((ret = pthread_kill( thread, sig )))
    {
        errno = ret;
        return -1;
    }
    return 0;
}

/* --- wineserver -------------------------------------------------------------------------------- */

extern int ps4_wineserver_main( int client_fd );

static void *wineserver_thread( void *arg )
{
    sigset_t all;

    /* every asynchronous signal is ntdll's: the server thread must never take one */
    sigfillset( &all );
    sigdelset( &all, SIGSEGV );
    sigdelset( &all, SIGBUS );
    sigdelset( &all, SIGILL );
    sigdelset( &all, SIGFPE );
    sigdelset( &all, SIGTRAP );
    sigdelset( &all, SIGXCPU );  /* the hang dump's */
    pthread_sigmask( SIG_BLOCK, &all, NULL );
    ps4_thread_register();
    ps4_wineserver_main( (int)(long)arg );
    fprintf( stderr, "wineps4: wineserver's main loop returned\n" );
    return NULL;
}

/* The desktop process's wineserver: a thread listening on its socket in the server directory, which
 * every program process (port/ps4_main.c) connects to. */
void ps4_start_wineserver_listening( void )
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 8 << 20 );
    if (pthread_create( &thread, &attr, wineserver_thread, (void *)(long)-1 ))
    {
        fprintf( stderr, "wineps4: could not start the wineserver thread\n" );
        abort();
    }
    pthread_attr_destroy( &attr );
}

/* --- Signal handlers, from the kernel -----------------------------------------------------------
 * The C library (FreeBSD's libthr) wraps every handler its sigaction installs in one of its own,
 * which finds its thread data through %fs; in 32-bit code %fs's base is the 32-bit TEB, and that
 * wrapper crashed before Wine's handler ran (v00.15). The system call installs the handler itself:
 * the kernel's trampoline calls it, and Wine's init_handler puts the C library's FS back first. */

static struct sigaction previous[64];  /* what each signal had before Wine's: ps4-kit's crash handler */
static unsigned char have_previous[64];

static int raw_sigaction( int sig, const struct sigaction *act, struct sigaction *old )
{
    long ret;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "=a" (ret), "=r" (failed)
                      : "a" (416 /* SYS_sigaction */), "D" ((long)sig), "S" (act), "d" (old)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (failed)
    {
        errno = (int)ret;
        return -1;
    }
    return 0;
}

int ps4_raw_sigaction( int sig, const struct sigaction *act, struct sigaction *old )
{
    struct sigaction before;

    if (sig > 0 && sig < 64 && !have_previous[sig] && act && !raw_sigaction( sig, NULL, &before ))
    {
        previous[sig] = before;
        have_previous[sig] = 1;
    }
    return raw_sigaction( sig, act, old );
}

/* A signal Wine's handler got on a thread that isn't Wine's (the desktop's, ps4-kit's): what was
 * there before has it, so a crash there reaches ps4-kit's crash log instead of Wine's handler, which
 * would look for a TEB the thread doesn't have. Called first in Wine's fault handlers, with the TEB
 * the signal stack would belong to; true when the signal was handed on. */
int ps4_foreign_signal( void *teb, int sig, siginfo_t *info, void *context )
{
    if (is_wine_teb( teb )) return 0;
    if (sig > 0 && sig < 64 && have_previous[sig])
    {
        const struct sigaction *p = &previous[sig];
        if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction)
        {
            p->sa_sigaction( sig, info, context );
            return 1;
        }
        if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN)
        {
            p->sa_handler( sig );
            return 1;
        }
    }
    /* nothing to hand it to: the system's own action, when it comes again */
    {
        struct sigaction dfl;
        memset( &dfl, 0, sizeof(dfl) );
        dfl.sa_handler = SIG_DFL;
        raw_sigaction( sig, &dfl, NULL );
    }
    return 1;
}

/* --- The end of the program --------------------------------------------------------------------
 * Wine's exit and _exit (ps4_wine_compat.h). A program process (port/ps4_main.c) ends with the
 * exit system call: ps4-kit's exit() closes the whole app (sceSystemServiceLoadExec "exit"), and the
 * desktop learns of the end from wait4. In the desktop process (wineserver's thread) it waits forever:
 * the desktop closes the app when the user leaves it. */

static long raw_syscall3( long number, long a, long b, long c );

int ps4_program_process;   /* this process runs a Windows program (set by ps4_main) */
int ps4_wineserver_stopped;  /* the desktop's wineserver ended (it failed to start) */
static int program_ended;  /* the hang dump stops then */

void ps4_wine_exit( int status )
{
    __atomic_store_n( &program_ended, 1, __ATOMIC_SEQ_CST );
    if (ps4_program_process)
    {
        fflush( NULL );
        for (;;) raw_syscall3( 1 /* SYS_exit */, status, 0, 0 );
    }
    fprintf( stderr, "wineps4: wineserver ended (%d), exit from %p\n", status, __builtin_return_address( 0 ));
    __atomic_store_n( &ps4_wineserver_stopped, 1, __ATOMIC_SEQ_CST );
    for (;;) sleep( 3600 );
}

/* --- Hang dump ---------------------------------------------------------------------------------
 * ps4-kit's thread dump signals with SIGUSR1, which is Wine's (and blocked during every server
 * call), so Wine's threads are dumped here with SIGXCPU, which Wine neither uses nor blocks. When
 * the program has not finished after WINEPS4_HANG_DUMP seconds (default 20, 0 = off), every thread
 * registered above writes its instruction pointer and the return addresses into the eboot on its
 * stack to /data/WinePS4/hang.log; twice more 30 s apart. Eboot addresses are as linked (the ELF
 * vaddrs: the image is linked at its base): symbolize with llvm-symbolizer --obj=<elf>. */

extern char __text_start[];  /* the linker script's: where the image starts (0x60000000) */
#define EBOOT_TEXT  ((uint64_t)__text_start)
#define DUMP_FRAMES 40
int32_t sceKernelQueryMemoryProtection( void *addr, void **start, void **end, uint32_t *prot );

static uint64_t text_end;  /* end of the eboot's code mapping */
static volatile int dump_done;
static uint64_t dump_rip, dump_frames[DUMP_FRAMES];
static int dump_count;

static int in_eboot( uint64_t addr )
{
    return addr >= EBOOT_TEXT + 8 && addr < text_end;
}

/* right after a call instruction: e8 rel32, or ff /2 with a 0, 1, 4 or 5 byte operand */
static int is_return_address( uint64_t addr )
{
    const unsigned char *p = (const unsigned char *)addr;

    if (!in_eboot( addr )) return 0;
    if (p[-5] == 0xe8) return 1;
    if (p[-2] == 0xff && (p[-1] & 0x38) == 0x10) return 1;
    if (p[-3] == 0xff && (p[-2] & 0x38) == 0x10) return 1;
    if (p[-6] == 0xff && (p[-5] & 0x38) == 0x10) return 1;
    if (p[-7] == 0xff && (p[-6] & 0x38) == 0x10) return 1;
    return 0;
}

/* Installed with the kernel's sigaction (ps4_raw_sigaction): it may interrupt 32-bit code, where %fs
 * isn't the C library's, so it calls nothing that needs it there (the memory query is libkernel's) */
static void dump_handler( int sig, siginfo_t *info, void *context )
{
    ucontext_t *uc = context;
    uint64_t rsp = uc->uc_mcontext.mc_rsp & ~7ull, *p, *end = (uint64_t *)(rsp + 0x10000);
    void *start, *map_end;
    uint32_t prot;
    unsigned short cs;
    int n = 0;

    dump_rip = uc->uc_mcontext.mc_rip;
    __asm__( "movw %%cs,%0" : "=r" (cs) );
    if (uc->uc_mcontext.mc_cs != cs)
        end = (uint64_t *)((rsp | 0x3fff) + 1);  /* 32-bit code: its 16 KiB page only */
    else if (!sceKernelQueryMemoryProtection( (void *)rsp, &start, &map_end, &prot ) && (uint64_t *)map_end < end)
        end = map_end;
    for (p = (uint64_t *)rsp; p < end && n < DUMP_FRAMES; p++)
        if (is_return_address( *p )) dump_frames[n++] = *p;
    dump_count = n;
    __atomic_store_n( &dump_done, 1, __ATOMIC_RELEASE );
}

static void dump_threads( const char *reason )
{
    FILE *f = fopen( "/data/WinePS4/hang.log", "a" );
    int i, j, waited;

    if (!f) return;
    fprintf( f, "==== %s (eboot addresses as linked: llvm-symbolizer --obj=<elf>)\n", reason );
    for (i = 0; i < MAX_THREADS; i++)
    {
        pthread_t thread;
        int tid;

        pthread_mutex_lock( &threads_lock );
        tid = threads[i].tid;
        thread = threads[i].thread;
        pthread_mutex_unlock( &threads_lock );
        if (!tid) continue;
        dump_done = 0;
        if (pthread_kill( thread, SIGXCPU )) { fprintf( f, "-- thread %d: gone\n", tid ); continue; }
        for (waited = 0; waited < 500 && !__atomic_load_n( &dump_done, __ATOMIC_ACQUIRE ); waited++) usleep( 1000 );
        if (!dump_done) { fprintf( f, "-- thread %d: no answer\n", tid ); continue; }
        if (in_eboot( dump_rip )) fprintf( f, "-- thread %d: rip elf 0x%llx\n   stack:", tid,
                                           (unsigned long long)dump_rip );
        else fprintf( f, "-- thread %d: rip 0x%llx (outside the eboot)\n   stack:", tid, (unsigned long long)dump_rip );
        for (j = 0; j < dump_count; j++) fprintf( f, " 0x%llx", (unsigned long long)dump_frames[j] );
        fprintf( f, "\n" );
    }
    fclose( f );
    fprintf( stderr, "wineps4: %s: thread stacks in /data/WinePS4/hang.log\n", reason );
}

static void *hang_watch_thread( void *arg )
{
    int seconds = (int)(long)arg, n;
    sigset_t set;
    char reason[64];

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    sleep( seconds );
    for (n = 0; n < 3 && !__atomic_load_n( &program_ended, __ATOMIC_ACQUIRE ); n++)
    {
        snprintf( reason, sizeof(reason), "still running after %d s", seconds + 30 * n );
        dump_threads( reason );
        sleep( 30 );
    }
    return NULL;
}

void ps4_start_hang_watch( void )
{
    const char *env = getenv( "WINEPS4_HANG_DUMP" );
    int seconds = env ? atoi( env ) : 20;
    struct sigaction sa;
    pthread_t thread;

    void *start, *end;
    uint32_t prot;

    if (seconds <= 0) return;
    if (sceKernelQueryMemoryProtection( (void *)EBOOT_TEXT, &start, &end, &prot )) return;
    text_end = (uint64_t)end;
    memset( &sa, 0, sizeof(sa) );
    sa.sa_sigaction = dump_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    ps4_raw_sigaction( SIGXCPU, &sa, NULL );
    /* (appended to: several program processes may write it; each dump says which) */
    if (!pthread_create( &thread, NULL, hang_watch_thread, (void *)(long)seconds )) pthread_detach( thread );
}

/* --- Built-in Unix libraries ------------------------------------------------------------------- */

struct ps4_unixlib
{
    const char *name;  /* the .so's file name */
    const struct { const char *name; void *addr; } *symbols;
};

/* the Unix libraries linked into the app (ps4_unixlibs.c) */
extern const struct ps4_unixlib ps4_unixlibs[];
#define unixlibs ps4_unixlibs
/* what dlopen( NULL ) gives: the main program, none of the libraries above */
static const struct ps4_unixlib main_program;

static char dl_error[256];

void *ps4_dlopen( const char *name, int flags )
{
    const char *base;
    const struct ps4_unixlib *lib;

    (void)flags;
    if (!name) return (void *)&main_program;
    base = strrchr( name, '/' );
    base = base ? base + 1 : name;
    for (lib = unixlibs; lib->name; lib++)
        if (!strcmp( lib->name, base ))
        {
            fprintf( stderr, "wineps4: library %s (built in)\n", base );
            return (void *)lib;
        }
    snprintf( dl_error, sizeof(dl_error), "%s: not built into WinePS4", base );
    fprintf( stderr, "wineps4: library %s: not built in\n", name );
    return NULL;
}

void *ps4_dlsym( void *handle, const char *name )
{
    const struct ps4_unixlib *lib;
    int i;

    /* one of the table's libraries, else the main program (dlopen( NULL ), RTLD_DEFAULT) */
    for (lib = unixlibs; lib->name; lib++) if (lib == handle) break;
    if (!lib->name) lib = &main_program;
    if (lib != &main_program && lib->symbols)
        for (i = 0; lib->symbols[i].name; i++)
            if (!strcmp( lib->symbols[i].name, name )) return lib->symbols[i].addr;
    snprintf( dl_error, sizeof(dl_error), "%s: no such symbol", name );
    if (lib != &main_program)
        fprintf( stderr, "wineps4: library %s has no %s\n", lib->name, name );
    return NULL;
}

int ps4_dlclose( void *handle ) { (void)handle; return 0; }

char *ps4_dlerror( void ) { return dl_error[0] ? dl_error : NULL; }

/* --- System calls libkernel does not export ---------------------------------------------------- */

static long raw_syscall3( long number, long a, long b, long c )
{
    long ret;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "=a" (ret), "=qm" (failed)
                      : "a" (number), "D" (a), "S" (b), "d" (c)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (failed)
    {
        errno = (int)ret;
        return -1;
    }
    return ret;
}

/* ps4-kit keeps a current directory of its own: relative paths are made absolute by its getcwd. */
static const char *absolute( const char *path, char *buf, size_t size )
{
    char cwd[1024];

    if (path[0] == '/') return path;
    if (!getcwd( cwd, sizeof(cwd) )) return path;
    snprintf( buf, size, "%s/%s", cwd, path );
    return buf;
}

int symlink( const char *target, const char *path )
{
    char buf[1100];
    return (int)raw_syscall3( 57, (long)target, (long)absolute( path, buf, sizeof(buf) ), 0 );
}

/* --- What the PS4 does not have ------------------------------------------------------------------ */

/* --- Processes: fork and exec (the process probe found both work) ------------------------------- */

extern char **environ;

/* FreeBSD's fork returns the other side's pid in both, with rdx = 1 in the child (libkernel has no
 * fork for apps). Wine's children only exec /app0/wineps4.bin after it. */
pid_t fork( void )
{
    long ret, child = 0;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %2"
                      : "=a" (ret), "+d" (child), "=r" (failed)
                      : "a" (2 /* SYS_fork */)
                      : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (failed)
    {
        errno = (int)ret;
        return -1;
    }
    return child ? 0 : (pid_t)ret;
}

/* --- Starting a program process from a program ------------------------------------------------ */

/* A system call that leaves errno alone: the child of a fork below runs nothing else */
static long raw_call3( long number, long a, long b, long c )
{
    long ret;
    __asm__ volatile( "syscall"
                      : "=a" (ret)
                      : "a" (number), "D" (a), "S" (b), "d" (c)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    return ret;
}

struct spawn_request
{
    const char *path;
    char      **argv, **envp;
    int         fd0, fd1;
    pid_t       pid;
    int         error;
};

static void *spawn_thread( void *arg )
{
    static const char failed_text[] = "wineps4: spawn: exec failed\n";
    struct spawn_request *req = arg;
    sigset_t all, none;
    long ret, child = 0;
    unsigned char failed;

    sigfillset( &all );
    sigemptyset( &none );
    pthread_sigmask( SIG_SETMASK, &all, NULL );  /* nothing runs in the child before its exec */
    __asm__ volatile( "syscall\n\tsetc %2"
                      : "=a" (ret), "+d" (child), "=r" (failed)
                      : "a" (2 /* SYS_fork */)
                      : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (!failed && child)
    {
        if (req->fd0 >= 0 && req->fd0 != 0) raw_call3( 90 /* SYS_dup2 */, req->fd0, 0, 0 );
        if (req->fd1 >= 0 && req->fd1 != 1) raw_call3( 90 /* SYS_dup2 */, req->fd1, 1, 0 );
        raw_call3( 340 /* SYS_sigprocmask */, 3 /* SIG_SETMASK */, (long)&none, 0 );
        raw_call3( 59 /* SYS_execve */, (long)req->path, (long)req->argv, (long)req->envp );
        raw_call3( 4 /* SYS_write */, 2, (long)failed_text, sizeof(failed_text) - 1 );
        raw_call3( 1 /* SYS_exit */, 127, 0, 0 );
    }
    req->pid = failed ? -1 : (pid_t)ret;
    req->error = failed ? (int)ret : 0;
    return NULL;
}

/* Forks and execs path from a thread of its own: Wine's threads run on stacks in direct memory,
 * which a forked child does not get (it died at once, before running anything: v00.29), and the
 * child touches nothing but what the request holds (made by the caller in malloc'd memory). */
pid_t ps4_spawn( const char *path, char **argv, char **envp, int fd0, int fd1 )
{
    struct spawn_request req = { path, argv, envp, fd0, fd1, -1, 0 };
    pthread_t thread;

    if (pthread_create( &thread, NULL, spawn_thread, &req ))
    {
        errno = EAGAIN;
        return -1;
    }
    pthread_join( thread, NULL );
    if (req.pid == -1) errno = req.error;
    return req.pid;
}

int execv( const char *path, char *const argv[] )
{
    char buf[1100];
    long ret = raw_syscall3( 59 /* SYS_execve */, (long)absolute( path, buf, sizeof(buf) ), (long)argv, (long)environ );
    return (int)ret;  /* (only on failure: errno is set) */
}

int execvp( const char *file, char *const argv[] )
{
    if (strchr( file, '/' )) return execv( file, argv );
    errno = ENOENT;  /* no PATH on the PS4 */
    return -1;
}

pid_t setsid( void ) { return getpid(); }

/* A signal to a thread of any process (wineserver's tkill): in this one by the thread table, else
 * the kernel's thr_kill2 */
int ps4_thread_kill2( int pid, int tid, int sig )
{
    if (pid == getpid()) return ps4_thread_kill( tid, sig );
    return (int)raw_syscall3( 481 /* SYS_thr_kill2 */, pid, tid, sig );
}
