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

#define MAX_THREADS 1024
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int tid; pthread_t thread; } threads[MAX_THREADS];

void ps4_thread_register( void )
{
    int tid = pthread_getthreadid_np(), i, free_slot = -1;

    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++)
    {
        if (threads[i].tid == tid) { free_slot = i; break; }
        if (!threads[i].tid && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0)
    {
        threads[free_slot].tid = tid;
        threads[free_slot].thread = pthread_self();
    }
    pthread_mutex_unlock( &threads_lock );
    if (free_slot < 0) fprintf( stderr, "wineps4: thread table full, thread %d cannot be signalled\n", tid );
}

void ps4_thread_unregister( void )
{
    int tid = pthread_getthreadid_np(), i;

    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++) if (threads[i].tid == tid) threads[i].tid = 0;
    pthread_mutex_unlock( &threads_lock );
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

/* Called by ntdll's server_connect(): returns ntdll's end of the server socket. */
int ps4_start_wineserver( void )
{
    static int started;
    pthread_attr_t attr;
    pthread_t thread;
    int fds[2];

    if (started) return -1;
    started = 1;
    if (socketpair( AF_UNIX, SOCK_STREAM, 0, fds ) == -1)
    {
        fprintf( stderr, "wineps4: socketpair for wineserver failed: %s\n", strerror( errno ));
        abort();
    }
    fcntl( fds[0], F_SETFD, FD_CLOEXEC );
    fcntl( fds[1], F_SETFD, FD_CLOEXEC );
    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 8 << 20 );
    if (pthread_create( &thread, &attr, wineserver_thread, (void *)(long)fds[1] ))
    {
        fprintf( stderr, "wineps4: could not start the wineserver thread\n" );
        abort();
    }
    pthread_attr_destroy( &attr );
    return fds[0];
}

/* --- The end of the program --------------------------------------------------------------------
 * Wine's exit and _exit (ps4_wine_compat.h): the first one (the program's) tells the desktop how
 * the program ended; every one then waits forever, so the desktop stays (wineserver's own exit
 * comes once the program is gone). The desktop closes the app when the user leaves it. */

extern void ps4_shell_program_ended( int status );

void ps4_wine_exit( int status )
{
    static int ended;

    if (!__atomic_exchange_n( &ended, 1, __ATOMIC_SEQ_CST ))
    {
        fprintf( stderr, "wineps4: the program ended (exit code %d)\n", status );
        ps4_shell_program_ended( status );
    }
    for (;;) sleep( 3600 );
}

/* --- Hang dump ---------------------------------------------------------------------------------
 * ps4-kit's thread dump signals with SIGUSR1, which is Wine's (and blocked during every server
 * call), so Wine's threads are dumped here with SIGXCPU, which Wine neither uses nor blocks. When
 * the program has not finished after WINEPS4_HANG_DUMP seconds (default 20, 0 = off), every thread
 * registered above writes its instruction pointer and the return addresses into the eboot on its
 * stack to /data/WinePS4/hang.log; twice more 30 s apart. Eboot addresses are relative to .text
 * (0x400000), as in ps4-kit's crash.log: symbolize with llvm-symbolizer --obj=<elf>. */

#define EBOOT_TEXT  0x400000
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

static void dump_handler( int sig, siginfo_t *info, void *context )
{
    ucontext_t *uc = context;
    uint64_t rsp = uc->uc_mcontext.mc_rsp & ~7ull, *p, *end = (uint64_t *)(rsp + 0x10000);
    void *start, *map_end;
    uint32_t prot;
    int n = 0;

    dump_rip = uc->uc_mcontext.mc_rip;
    if (!sceKernelQueryMemoryProtection( (void *)rsp, &start, &map_end, &prot ) && (uint64_t *)map_end < end)
        end = map_end;
    for (p = (uint64_t *)rsp; p < end && n < DUMP_FRAMES; p++)
        if (is_return_address( *p )) dump_frames[n++] = *p - EBOOT_TEXT;
    dump_count = n;
    __atomic_store_n( &dump_done, 1, __ATOMIC_RELEASE );
}

static void dump_threads( const char *reason )
{
    FILE *f = fopen( "/data/WinePS4/hang.log", "a" );
    int i, j, waited;

    if (!f) return;
    fprintf( f, "==== %s (eboot addresses relative to .text)\n", reason );
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
                                           (unsigned long long)(dump_rip - EBOOT_TEXT) );
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
    for (n = 0; n < 3; n++)
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
    sigaction( SIGXCPU, &sa, NULL );
    unlink( "/data/WinePS4/hang.log" );
    if (!pthread_create( &thread, NULL, hang_watch_thread, (void *)(long)seconds )) pthread_detach( thread );
}

/* --- Built-in Unix libraries ------------------------------------------------------------------- */

struct ps4_unixlib
{
    const char *name;  /* the .so's file name */
    const struct { const char *name; void *addr; } *symbols;
};

/* Unix libraries of builtin DLLs are added here as they are ported (win32u.so, winevulkan.so, ...). */
static const struct ps4_unixlib unixlibs[] =
{
    { NULL, NULL }
};

static char dl_error[256];

void *ps4_dlopen( const char *name, int flags )
{
    const char *base;
    const struct ps4_unixlib *lib;

    (void)flags;
    if (!name) return (void *)unixlibs;  /* the main program */
    base = strrchr( name, '/' );
    base = base ? base + 1 : name;
    for (lib = unixlibs; lib->name; lib++) if (!strcmp( lib->name, base )) return (void *)lib;
    snprintf( dl_error, sizeof(dl_error), "%s: not built into WinePS4", base );
    return NULL;
}

void *ps4_dlsym( void *handle, const char *name )
{
    const struct ps4_unixlib *lib = handle;
    int i;

    if (lib && lib != unixlibs && lib->symbols)
        for (i = 0; lib->symbols[i].name; i++)
            if (!strcmp( lib->symbols[i].name, name )) return lib->symbols[i].addr;
    snprintf( dl_error, sizeof(dl_error), "%s: no such symbol", name );
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

pid_t fork( void ) { errno = ENOSYS; return -1; }
int execv( const char *path, char *const argv[] ) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int execvp( const char *file, char *const argv[] ) { (void)file; (void)argv; errno = ENOSYS; return -1; }
pid_t setsid( void ) { return getpid(); }
