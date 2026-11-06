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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <machine/sysarch.h>

#include "ps4_line.h"

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
/* (stack: its Unix stack's bounds, for the dump's handler: asking the kernel there stood with a
 * desktop whose address space stood, v00.64) */
static struct { int tid; pthread_t thread; void *teb; uint64_t stack_lo, stack_hi; char name[20]; } threads[MAX_THREADS];
static __thread char thread_name[20];  /* (its name, given before or after its registration) */
static pthread_key_t thread_key;
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;

void ps4_thread_unregister( void );
static void thread_ended( void *arg ) { ps4_thread_unregister(); }
static void make_thread_key( void ) { pthread_key_create( &thread_key, thread_ended ); }

void ps4_thread_register( void )
{
    int tid = pthread_getthreadid_np(), i, free_slot = -1;
    uint64_t lo = 0, hi = 0;
    pthread_attr_t attr;
    void *stack;
    size_t size;

    if (!pthread_attr_init( &attr ))
    {
        if (!pthread_attr_get_np( pthread_self(), &attr ) && !pthread_attr_getstack( &attr, &stack, &size ))
            lo = (uint64_t)(uintptr_t)stack, hi = lo + size;
        pthread_attr_destroy( &attr );
    }
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
        threads[free_slot].stack_lo = lo;
        threads[free_slot].stack_hi = hi;
        memcpy( threads[free_slot].name, thread_name, sizeof(thread_name) );
        __atomic_store_n( &threads[free_slot].tid, tid, __ATOMIC_RELEASE );
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
            threads[i].name[0] = 0;
            __atomic_store_n( &threads[i].teb, NULL, __ATOMIC_RELEASE );
        }
    pthread_mutex_unlock( &threads_lock );
}

/* the calling thread's name in its place in the table, for the dumps (port/ps4_kstate.c's
 * ps4_thread_name gives it; one given before the thread registers is kept for its registration) */
void ps4_thread_table_name( const char *name )
{
    const int tid = pthread_getthreadid_np();
    int i;

    snprintf( thread_name, sizeof(thread_name), "%s", name );
    pthread_mutex_lock( &threads_lock );
    for (i = 0; i < MAX_THREADS; i++)
        if (threads[i].tid == tid) memcpy( threads[i].name, thread_name, sizeof(thread_name) );
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
extern void ps4_thread_name( const char *name );  /* (port/ps4_kstate.c) */

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
    ps4_thread_name( "wineserver" );
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

int ps4_control_fd = -1;  /* a local process's socket to the desktop (port/ps4_spawn_desk.c) */

/* a local process tells the desktop its exit code: one system call, nothing that may lock */
static void tell_exit( int status )
{
    const struct { uint32_t magic; int32_t code; } msg = { 0x54495845 /* "EXIT" */, status };

    if (ps4_control_fd >= 0) raw_syscall3( 4 /* SYS_write */, ps4_control_fd, (long)&msg, sizeof(msg) );
}

/* ends this process now, with no flush and no lock (a program process only) */
void ps4_process_exit_now( int status )
{
    tell_exit( status );
    for (;;) raw_syscall3( 1 /* SYS_exit */, status, 0, 0 );
}

void ps4_wine_exit( int status )
{
    __atomic_store_n( &program_ended, 1, __ATOMIC_SEQ_CST );
    if (ps4_program_process)
    {
        fflush( NULL );
        tell_exit( status );
        for (;;) raw_syscall3( 1 /* SYS_exit */, status, 0, 0 );
    }
    fprintf( stderr, "wineps4: wineserver ended (%d), exit from %p\n", status, __builtin_return_address( 0 ));
    __atomic_store_n( &ps4_wineserver_stopped, 1, __ATOMIC_SEQ_CST );
    for (;;) sleep( 3600 );
}

/* --- Hang dump ---------------------------------------------------------------------------------
 * ps4-kit's thread dump signals with SIGUSR1, which is Wine's (and blocked during every server
 * call), so Wine's threads are dumped here with SIGXCPU, which Wine neither uses nor blocks. When
 * the program has not finished after WINEPS4_HANG_DUMP seconds (Settings > Diagnostics > Hang report
 * sets 20; off without it: services, explorer.exe and windowed programs run on), every thread
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
static int dump_heap, dump_stdio;  /* how deep the thread was in the heap and in stdio (-1: not known) */
extern int ps4_heap_depth( void );   /* ps4-kit's, thread-local */
extern int ps4_stdio_depth( void );

/* the desktop's windows' lock (shell/winwindows.cpp): who holds it, and where it was taken (the
 * freeze report's: v00.63's desktop stood waiting for it) */
volatile int ps4_windows_lock_holder;
void *volatile ps4_windows_lock_caller;

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

/* the top of the calling thread's Unix stack when rsp is on it (the table's, from its registration),
 * else 0: no system call but the kernel's thread id */
static uint64_t registered_stack_top( uint64_t rsp )
{
    long tid = 0;
    int i;

    if (raw_syscall3( 432 /* SYS_thr_self */, (long)&tid, 0, 0 ) == -1) return 0;
    for (i = 0; i < MAX_THREADS; i++)
        if (__atomic_load_n( &threads[i].tid, __ATOMIC_ACQUIRE ) == (int)tid)
            return rsp >= threads[i].stack_lo && rsp < threads[i].stack_hi ? threads[i].stack_hi : 0;
    return 0;
}

/* Installed with the kernel's sigaction (ps4_raw_sigaction): it may interrupt 32-bit code, where %fs
 * isn't the C library's, so it calls nothing that needs it there (the memory query is libkernel's,
 * and only for a stack the thread's registration didn't tell) */
static void dump_handler( int sig, siginfo_t *info, void *context )
{
    ucontext_t *uc = context;
    uint64_t rsp = uc->uc_mcontext.mc_rsp & ~7ull, *p, *end = (uint64_t *)(rsp + 0x10000), top;
    void *start, *map_end;
    uint32_t prot;
    unsigned short cs;
    int n = 0;

    dump_rip = uc->uc_mcontext.mc_rip;
    dump_heap = dump_stdio = -1;
    __asm__( "movw %%cs,%0" : "=r" (cs) );
    if (uc->uc_mcontext.mc_cs != cs)
        end = (uint64_t *)((rsp | 0x3fff) + 1);  /* 32-bit code: its 16 KiB page only */
    else
    {
        /* (64-bit code: %fs is the C library's, its thread-local counts are this thread's) */
        dump_heap = ps4_heap_depth();
        dump_stdio = ps4_stdio_depth();
        if ((top = registered_stack_top( rsp )))
        {
            if ((uint64_t *)top < end) end = (uint64_t *)top;
        }
        else if (!sceKernelQueryMemoryProtection( (void *)rsp, &start, &map_end, &prot ) && (uint64_t *)map_end < end)
            end = map_end;
    }
    for (p = (uint64_t *)rsp; p < end && n < DUMP_FRAMES; p++)
        if (is_return_address( *p )) dump_frames[n++] = *p;
    dump_count = n;
    __atomic_store_n( &dump_done, 1, __ATOMIC_RELEASE );
}

static void dump_threads( const char *reason, int notice )
{
    FILE *f = fopen( "/data/WinePS4/hang.log", "a" );
    int i, j, waited;

    if (!f) return;
    fprintf( f, "==== %s (eboot addresses as linked: llvm-symbolizer --obj=<elf>)\n", reason );
    for (i = 0; i < MAX_THREADS; i++)
    {
        pthread_t thread;
        char name[sizeof(threads[0].name)], who[48];
        int tid;

        pthread_mutex_lock( &threads_lock );
        tid = threads[i].tid;
        thread = threads[i].thread;
        memcpy( name, threads[i].name, sizeof(name) );
        pthread_mutex_unlock( &threads_lock );
        if (!tid) continue;
        name[sizeof(name) - 1] = 0;
        if (name[0]) snprintf( who, sizeof(who), "%d '%s'", tid, name );
        else snprintf( who, sizeof(who), "%d", tid );
        dump_done = 0;
        if (pthread_kill( thread, SIGXCPU )) { fprintf( f, "-- thread %s: gone\n", who ); continue; }
        for (waited = 0; waited < 500 && !__atomic_load_n( &dump_done, __ATOMIC_ACQUIRE ); waited++) usleep( 1000 );
        if (!dump_done) { fprintf( f, "-- thread %s: no answer\n", who ); continue; }
        if (in_eboot( dump_rip )) fprintf( f, "-- thread %s: rip elf 0x%llx\n   stack:", who,
                                           (unsigned long long)dump_rip );
        else fprintf( f, "-- thread %s: rip 0x%llx (outside the eboot)\n   stack:", who, (unsigned long long)dump_rip );
        for (j = 0; j < dump_count; j++) fprintf( f, " 0x%llx", (unsigned long long)dump_frames[j] );
        fprintf( f, "\n" );
    }
    fclose( f );
    if (notice) fprintf( stderr, "wineps4: %s: thread stacks in /data/WinePS4/hang.log\n", reason );
}

static int install_dump_handler( void )
{
    struct sigaction sa;
    void *start, *end;
    uint32_t prot;

    if (text_end) return 1;
    if (sceKernelQueryMemoryProtection( (void *)EBOOT_TEXT, &start, &end, &prot )) return 0;
    text_end = (uint64_t)end;
    memset( &sa, 0, sizeof(sa) );
    sa.sa_sigaction = dump_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    ps4_raw_sigaction( SIGXCPU, &sa, NULL );
    return 1;
}

/* The desktop's freeze report (port/ps4_main.c): its threads into hang.log, with nothing that may wait
 * for what a stuck thread holds: no stdio and no malloc (v00.62's stopped in fopen), no printf at all
 * (snprintf takes ps4-kit's stdio lock through vfprintf: v00.63's stopped after four threads), the
 * table read without its lock, the signal sent by the kernel's thr_kill, the waits the kernel's
 * nanosleep, the file opened by a system call. The holder of the windows' lock first; then each
 * thread, with how deep it was in the heap and in stdio (waiting there or holding the lock). */
void ps4_dump_threads_quiet( const char *reason )
{
    const struct { long sec, nsec; } tick = { 0, 1000000 };
    struct ps4_line l;
    int i, j, waited, holder;
    long fd;

    if (!install_dump_handler()) return;
    /* (FreeBSD's O_WRONLY | O_APPEND | O_CREAT: the C library's headers have Linux's numbers, its open
     * converts them) */
    if ((fd = raw_syscall3( 5 /* SYS_open */, (long)"/data/WinePS4/hang.log", 0x0001 | 0x0008 | 0x0200, 0666 )) == -1)
        return;
    l.len = 0;
    ps4_line_str( &l, "==== " );
    for (j = 0; reason[j] && reason[j] != '\n' && l.len < (int)sizeof(l.text) - 1; j++) l.text[l.len++] = reason[j];
    ps4_line_str( &l, " (eboot addresses as linked: llvm-symbolizer --obj=<elf>)\n-- the windows' lock: " );
    if ((holder = ps4_windows_lock_holder))
    {
        ps4_line_str( &l, "held by thread " );
        ps4_line_num( &l, holder );
        ps4_line_str( &l, ", taken at " );
        ps4_line_hex( &l, (unsigned long long)(uintptr_t)ps4_windows_lock_caller );
    }
    else ps4_line_str( &l, "free" );
    ps4_line_str( &l, "\n" );
    raw_syscall3( 4 /* SYS_write */, fd, (long)l.text, l.len );
    for (i = 0; i < MAX_THREADS; i++)
    {
        const int tid = __atomic_load_n( &threads[i].tid, __ATOMIC_ACQUIRE );

        if (!tid) continue;
        l.len = 0;
        ps4_line_str( &l, "-- thread " );
        ps4_line_num( &l, tid );
        {
            char name[sizeof(threads[0].name)];

            memcpy( name, threads[i].name, sizeof(name) );
            name[sizeof(name) - 1] = 0;
            if (name[0])
            {
                ps4_line_str( &l, " '" );
                ps4_line_str( &l, name );
                ps4_line_str( &l, "'" );
            }
        }
        __atomic_store_n( &dump_done, 0, __ATOMIC_RELEASE );
        if (raw_syscall3( 433 /* SYS_thr_kill */, tid, SIGXCPU, 0 ) == -1)
            ps4_line_str( &l, ": gone" );
        else
        {
            /* (a line a thread, written at once: the user may close the app before the last) */
            for (waited = 0; waited < 50 && !__atomic_load_n( &dump_done, __ATOMIC_ACQUIRE ); waited++)
                raw_syscall3( 240 /* SYS_nanosleep */, (long)&tick, 0, 0 );
            if (!__atomic_load_n( &dump_done, __ATOMIC_ACQUIRE ))
                ps4_line_str( &l, ": no answer" );
            else
            {
                ps4_line_str( &l, in_eboot( dump_rip ) ? ": rip elf " : ": rip " );
                ps4_line_hex( &l, dump_rip );
                if (!in_eboot( dump_rip )) ps4_line_str( &l, " (outside the eboot)" );
                if (dump_heap > 0) ps4_line_str( &l, ", in the heap" );
                if (dump_stdio > 0) ps4_line_str( &l, ", in stdio" );
                if (tid == holder) ps4_line_str( &l, ", holds the windows' lock" );
                ps4_line_str( &l, "\n   stack:" );
                for (j = 0; j < dump_count; j++)
                {
                    ps4_line_str( &l, " " );
                    ps4_line_hex( &l, dump_frames[j] );
                }
            }
        }
        ps4_line_str( &l, "\n" );
        raw_syscall3( 4 /* SYS_write */, fd, (long)l.text, l.len );
    }
    raw_syscall3( 6 /* SYS_close */, fd, 0, 0 );
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
        dump_threads( reason, 1 );
        sleep( 30 );
    }
    return NULL;
}

/* The process's threads into hang.log now (the desktop's, its wineserver thread among them, when the
 * server stops taking the desktop's input: server/ps4_input.inc), when Settings > Diagnostics > Hang
 * report is on */
void ps4_dump_threads_now( const char *reason )
{
    const char *env = getenv( "WINEPS4_HANG_DUMP" );

    if (!env || atoi( env ) <= 0) return;
    if (install_dump_handler()) dump_threads( reason, 1 );
}

void ps4_start_hang_watch( void )
{
    const char *env = getenv( "WINEPS4_HANG_DUMP" );
    int seconds = env ? atoi( env ) : 0;
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

/* Anonymous shared memory for wineserver's sections (server/mapping.c): FreeBSD's shm_open( SHM_ANON ),
 * by its system call (libkernel's shm_open would take SHM_ANON for a path). Not a file: the PS4 counts
 * the files the app has open, all its processes together, to about 250, and each section was one (a
 * temporary file in the server's folder: Chromium's hundred of them used the budget up, v00.56). -1 when
 * the kernel says no: temporary files then, as before (for good when the first one fails). */
static int anon_shm( unsigned long long size )
{
    static int works = -1;
    long fd;

    if (!works) return -1;
    if ((fd = raw_syscall3( 482 /* SYS_shm_open */, 1 /* SHM_ANON */, O_RDWR, 0600 )) == -1)
    {
        if (works == -1) fprintf( stderr, "wineps4: sections: no anonymous shared memory (%s): temporary files\n", strerror( errno ) );
        works = 0;
        return -1;
    }
    if (ftruncate( (int)fd, (off_t)size ) == -1)
    {
        const int error = errno;

        close( (int)fd );
        if (works == -1)
        {
            fprintf( stderr, "wineps4: sections: anonymous shared memory did not grow to %llu bytes (%s): temporary files\n",
                     size, strerror( error ) );
            works = 0;
        }
        return -1;
    }
    if (works == -1) fprintf( stderr, "wineps4: sections in anonymous shared memory (not files of the app's budget)\n" );
    works = 1;
    return (int)fd;
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

/* What the child of ps4_spawn reads: all of it in malloc'd memory, which a forked child has (Wine's
 * thread stacks are in direct memory, which it doesn't: v00.29/30) */
struct spawn_request
{
    const char *path;          /* 0 */
    char      **argv;          /* 8 */
    char      **envp;          /* 16 */
    int         fd0, fd1;      /* 24, 28 */
    const void *mask;          /* 32: the signal mask the program starts with (none blocked) */
    const char *text;          /* 40: said when the exec fails */
    long        text_len;      /* 48 */
    unsigned char empty_mask[16];  /* 56 */
    int         control;       /* 72: ps4_spawn_outside: the socket to the desktop (descriptor 3 there) */
    int         leave_budget;  /* 76: ps4_spawn_outside: budget_set( 0 ) before the exec */
    int         close_limit;   /* 80: ps4_spawn_outside: descriptors 4 to this one closed */
};

/* vfork (or fork) and exec, the child running only this code: no stack, nothing but system calls
 * on the request (FreeBSD 9 on the PS4 clobbers rcx, r8-r11 in a syscall; rbx survives). The child
 * never ends before its exec but when the exec fails (see "More than 7 processes" for why) */
static long fork_exec( long syscall_number, struct spawn_request *req, long *child_pid )
{
    long ret, flag;

    __asm__ volatile( "syscall\n\t"
                      "jnc 1f\n\t"
                      "movq $-1,%%rdx\n\t"          /* failed: rax = errno */
                      "jmp 9f\n"
                      "1:\ttestq %%rdx,%%rdx\n\t"   /* rdx: 0 in the parent, 1 in the child */
                      "jz 9f\n\t"
                      /* the child */
                      "movl 24(%%rbx),%%edi\n\t"    /* dup2( fd0, 0 ) unless it is 0 or none */
                      "testl %%edi,%%edi\n\t"
                      "jle 2f\n\t"
                      "xorl %%esi,%%esi\n\t"
                      "movl $90,%%eax\n\t"
                      "syscall\n"
                      "2:\tmovl 28(%%rbx),%%edi\n\t" /* dup2( fd1, 1 ) unless it is 1 or none */
                      "cmpl $1,%%edi\n\t"
                      "je 3f\n\t"
                      "testl %%edi,%%edi\n\t"
                      "js 3f\n\t"
                      "movl $1,%%esi\n\t"
                      "movl $90,%%eax\n\t"
                      "syscall\n"
                      "3:\tmovl $3,%%edi\n\t"         /* sigprocmask( SIG_SETMASK, mask, NULL ) */
                      "movq 32(%%rbx),%%rsi\n\t"
                      "xorl %%edx,%%edx\n\t"
                      "movl $340,%%eax\n\t"
                      "syscall\n\t"
                      "movq 0(%%rbx),%%rdi\n\t"     /* execve( path, argv, envp ) */
                      "movq 8(%%rbx),%%rsi\n\t"
                      "movq 16(%%rbx),%%rdx\n\t"
                      "movl $59,%%eax\n\t"
                      "syscall\n\t"
                      "movl $2,%%edi\n\t"           /* it failed: write( 2, text, len ), exit( 127 ) */
                      "movq 40(%%rbx),%%rsi\n\t"
                      "movq 48(%%rbx),%%rdx\n\t"
                      "movl $4,%%eax\n\t"
                      "syscall\n\t"
                      "movl $127,%%edi\n\t"
                      "movl $1,%%eax\n\t"
                      "syscall\n"
                      "9:"
                      : "=a" (ret), "=d" (flag)
                      : "a" (syscall_number), "b" (req)
                      : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc" );
    if (flag < 0) return -(int)ret;
    *child_pid = ret;
    return 0;
}

/* --- More than 7 processes ----------------------------------------------------------------------
 * The PS4 runs 7 local processes for an app (sceSystemServiceAddLocalProcess refuses the 8th, nested
 * or not, whatever their memory: PS5_MihawkOS measured the same on the PS5). Processes forked by the
 * app are not counted, but closing the app from the PS4 menu tore the app down under them (a kernel
 * panic, v00.30-00.42). On the PS5 the payload loader starts processes outside every app: the child
 * leaves its budget (budget_set 571 with 0), so it is no app's; v00.50's test found a child of this
 * app may do it on the PS4 too (ps4_spawn_outside, below).
 *
 * A vfork child must exec, never end on its own: it ends on its parent's address space, which the PS4
 * takes as the end of the desktop's own. That test's child (v00.50 to v00.69: budget_set, then exit)
 * freed the app's GPU address space 0.5 s into every session (the kernel's log: "dmem_handle_vmspace_
 * exited" with ptype 2 and the desktop's map, then "gbase_free_vm_space_2 has mapped memory vmid:1",
 * which a game's last process prints as the game closes; the app's own end then found "no vm space").
 * The desktop's screen and every program's GPU memory went on in a space the kernel had freed: most
 * likely why the app stood as a program's process ended (v00.68, v00.69). A failed exec still ends the
 * child that way; the app's own path never fails to exec. */

/* Starts path (the app) as a new process: vfork, which the PS4 may not have (then fork). envp,
 * argv and their strings must be malloc'd (see struct spawn_request). */
pid_t ps4_spawn( const char *path, char **argv, char **envp, int fd0, int fd1 )
{
    static const char failed_text[] = "wineps4: spawn: exec failed\n";
    static int no_vfork;
    struct spawn_request *req = calloc( 1, sizeof(*req) );
    sigset_t all, old;
    long err, pid = -1;

    if (!req)
    {
        errno = ENOMEM;
        return -1;
    }
    req->path = path;
    req->argv = argv;
    req->envp = envp;
    req->fd0 = fd0;
    req->fd1 = fd1;
    req->mask = req->empty_mask;
    req->text = failed_text;
    req->text_len = sizeof(failed_text) - 1;
    sigfillset( &all );
    pthread_sigmask( SIG_SETMASK, &all, &old );  /* nothing runs in the child before its exec */
    err = no_vfork ? ENOSYS : -fork_exec( 66 /* SYS_vfork */, req, &pid );
    if (err == ENOSYS)
    {
        if (!no_vfork) fprintf( stderr, "wineps4: no vfork, fork instead\n" );
        no_vfork = 1;
        err = -fork_exec( 2 /* SYS_fork */, req, &pid );
    }
    pthread_sigmask( SIG_SETMASK, &old, NULL );
    free( req );
    if (err)
    {
        errno = (int)err;
        return -1;
    }
    return (pid_t)pid;
}

/* --- Past the PS4's 7: a process outside the system's own of the app ------------------------------
 * The desktop starts it itself (port/ps4_spawn_desk.c): vfork, and the child, with only system
 * calls on the request: its socket to the desktop as descriptor 3, every other descriptor closed (a
 * program's socket to the wineserver held open by another process would keep the server from
 * seeing that program end; one by one: a system call the kernel doesn't have, closefrom, would be
 * a SIGSYS), out of the app's budget (budget_set 571 with 0: the PS4 lets a child of the app do it,
 * v00.50's test; the PS5's payload loader does the same) if asked, then the exec. Its pid, or -1. */
static long outside_exec( struct spawn_request *req, long *child_pid )
{
    long ret, flag;

    __asm__ volatile( "syscall\n\t"
                      "jnc 1f\n\t"
                      "movq $-1,%%rdx\n\t"          /* failed: rax = errno */
                      "jmp 9f\n"
                      "1:\ttestq %%rdx,%%rdx\n\t"   /* rdx: 0 in the parent, 1 in the child */
                      "jz 9f\n\t"
                      /* the child */
                      "movl 72(%%rbx),%%edi\n\t"    /* dup2( control, 3 ) */
                      "movl $3,%%esi\n\t"
                      "movl $90,%%eax\n\t"
                      "syscall\n\t"
                      "movl $4,%%r12d\n"            /* close( 4 ... close_limit ) */
                      "5:\tcmpl 80(%%rbx),%%r12d\n\t"
                      "ja 4f\n\t"
                      "movl %%r12d,%%edi\n\t"
                      "movl $6,%%eax\n\t"
                      "syscall\n\t"
                      "incl %%r12d\n\t"
                      "jmp 5b\n"
                      "4:\tcmpl $0,76(%%rbx)\n\t"   /* budget_set( 0 ) */
                      "je 6f\n\t"
                      "xorl %%edi,%%edi\n\t"
                      "movl $571,%%eax\n\t"
                      "syscall\n"
                      "6:\tmovl $3,%%edi\n\t"       /* sigprocmask( SIG_SETMASK, mask, NULL ) */
                      "movq 32(%%rbx),%%rsi\n\t"
                      "xorl %%edx,%%edx\n\t"
                      "movl $340,%%eax\n\t"
                      "syscall\n\t"
                      "movq 0(%%rbx),%%rdi\n\t"     /* execve( path, argv, envp ) */
                      "movq 8(%%rbx),%%rsi\n\t"
                      "movq 16(%%rbx),%%rdx\n\t"
                      "movl $59,%%eax\n\t"
                      "syscall\n\t"
                      "movl $2,%%edi\n\t"           /* it failed: write( 2, text, len ), exit( 127 ) */
                      "movq 40(%%rbx),%%rsi\n\t"
                      "movq 48(%%rbx),%%rdx\n\t"
                      "movl $4,%%eax\n\t"
                      "syscall\n\t"
                      "movl $127,%%edi\n\t"
                      "movl $1,%%eax\n\t"
                      "syscall\n"
                      "9:"
                      : "=a" (ret), "=d" (flag)
                      : "a" (66 /* SYS_vfork */), "b" (req)
                      : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "memory", "cc" );
    if (flag < 0) return -(int)ret;
    *child_pid = ret;
    return 0;
}

pid_t ps4_spawn_outside( const char *path, char **argv, char **envp, int control, int leave_budget )
{
    static const char failed_text[] = "wineps4: spawn: exec failed (a process outside the PS4's 7)\n";
    struct spawn_request *req = calloc( 1, sizeof(*req) );
    struct rlimit files;
    sigset_t all, old;
    long err, pid = -1;

    if (!req)
    {
        errno = ENOMEM;
        return -1;
    }
    if (control == 3) fcntl( control, F_SETFD, 0 );  /* (dup2 of 3 onto itself keeps close-on-exec) */
    /* (FreeBSD's RLIMIT_NOFILE: the C library's headers have Linux's number) */
    req->close_limit = !getrlimit( 8, &files ) && files.rlim_cur < 65536 ? (int)files.rlim_cur : 16384;
    req->path = path;
    req->argv = argv;
    req->envp = envp;
    req->mask = req->empty_mask;
    req->text = failed_text;
    req->text_len = sizeof(failed_text) - 1;
    req->control = control;
    req->leave_budget = leave_budget;
    sigfillset( &all );
    pthread_sigmask( SIG_SETMASK, &all, &old );  /* nothing runs in the child before its exec */
    err = -outside_exec( req, &pid );
    pthread_sigmask( SIG_SETMASK, &old, NULL );
    free( req );
    if (err)
    {
        errno = (int)err;
        return -1;
    }
    return (pid_t)pid;
}

/* this process's budget (budget_getid 609): 0 outside every app's */
long ps4_budget_id( void )
{
    return raw_syscall3( 609, 0, 0, 0 );
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

/* wineserver's sections (server/mapping.c), marked while it runs (port/ps4_kstate.c) */
extern void ps4_call_enter( const char *what );
extern void ps4_call_leave( void );

int ps4_anon_shm( unsigned long long size )
{
    int ret;

    ps4_call_enter( "shared memory for a section" );
    ret = anon_shm( size );
    ps4_call_leave();
    return ret;
}
