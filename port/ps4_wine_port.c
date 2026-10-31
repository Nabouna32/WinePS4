/* WinePS4: what Wine's Unix side expects from the system and the PS4 provides differently.
 *
 *  - wineserver as a thread of this process, reached through a socket pair (no fork/exec)
 *  - signals to a thread by its kernel thread id (wineserver's SuspendThread and APCs)
 *  - FreeBSD's amd64_{get,set}_{gs,fs}base on sysarch
 *  - dlopen/dlsym for the Unix libraries, which are linked into the eboot
 *  - the FreeBSD system calls libkernel does not export (symlink, readlink), made directly
 *  - stubs for what has no PS4 equivalent (fork, exec, ptrace) */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
    pthread_sigmask( SIG_BLOCK, &all, NULL );
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
