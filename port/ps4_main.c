/* WinePS4: the app's entry point - the desktop (shell/) on the main thread, and Wine's loader
 * (loader/main.c) on a thread of its own for the program picked there.
 *
 *  /data/WinePS4/wine.log   everything Wine writes to stdout/stderr, and the program's console output
 *                           (also shown in the desktop's Terminal), each line with the seconds since
 *                           the app started
 *  /data/WinePS4/env.txt    optional KEY=VALUE lines put in the environment first (WINEDEBUG=+server);
 *                           the desktop's Settings write its switches there
 *  /data/WinePS4/launch.txt the program to run now (one argument per line): the desktop writes it,
 *                           then restarts the app; removed once read
 *  /data/WinePS4/run.txt    optional, for testing: a program to run at every start, like launch.txt
 *  /data/WinePS4/prefix     the Wine prefix, copied from the package (/app0/wine/prefix) on first run
 *  /data/WinePS4/crash.log  ps4-kit's report of a crash (boot-trace.log: the start up to it)
 *  /data/WinePS4/hang.log   where Wine's threads are when the program has not finished after
 *                           WINEPS4_HANG_DUMP seconds (Settings > Diagnostics > Hang report; off
 *                           without it: services, explorer.exe and windowed programs run on)
 *
 * The programs are processes of their own: the desktop has the PS4 start each one as a local process
 * of the app (the ones programs start too: port/ps4_spawn_desk.c), which the system suspends and ends
 * with the app; Power > Exit ends them first (ps4_end_programs). Windows' background programs, and
 * any program once the PS4's 7 run, the desktop starts itself, outside the app's own processes: with
 * regular flexible memory only, ending with the desktop. Where the PS4 refuses local processes, they
 * are the desktop's forked children, which end themselves when the desktop is gone (the lifeline). */
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ps4_kit_diag.h"
#include "ps4_kit.h"
#include "../shell/shell.h"
#include "include/ps4_display.h"
#include "include/ps4_line.h"

#define DESKTOP_LOCK_FILE "/data/WinePS4/.desktop-lock"  /* locked by the desktop while it runs */
#define DESKTOP_PID_FILE  "/data/WinePS4/.desktop-pid"   /* the desktop's pid, written as it starts */

/* a system call before anything is set up: no errno, no libc (-errno when it failed) */
static long early_syscall3( long number, long a, long b, long c )
{
    long ret;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "=a" (ret), "=qm" (failed)
                      : "a" (number), "D" (a), "S" (b), "d" (c)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    return failed ? -ret : ret;
}

static long desktop_lock = -1;  /* the desktop's descriptor of DESKTOP_LOCK_FILE, locked */

/* A program process: the desktop holds the lock. Otherwise this is the desktop, which takes it now
 * (FreeBSD flags and numbers). (Its parent no longer tells: the PS4 starts the programs, as local
 * processes of the app.) */
static int is_program_process( void )
{
    long fd = early_syscall3( 5 /* SYS_open */, (long)DESKTOP_LOCK_FILE, 0x0202 /* O_RDWR | O_CREAT */, 0666 );
    long ret;

    if (fd < 0) return 0;  /* (no data folder yet: the very first start, the desktop's) */
    if ((ret = early_syscall3( 131 /* SYS_flock */, fd, 2 | 4 /* LOCK_EX | LOCK_NB */, 0 )) == 0)
    {
        early_syscall3( 92 /* SYS_fcntl */, fd, 2 /* F_SETFD */, 1 /* FD_CLOEXEC */ );
        desktop_lock = fd;
        return 0;
    }
    early_syscall3( 6 /* SYS_close */, fd, 0, 0 );
    return ret == -35 /* EWOULDBLOCK */;
}

int ps4_outside_process;  /* the desktop started this one itself, outside the PS4's 7 (port/ps4_wine_vm.c) */

/* A program process the desktop started itself (past the PS4's 7, or forked where the PS4 starts
 * none: port/ps4_spawn_desk.c): its parent is the desktop. The PS4 starts the local processes (their
 * parent is the system's). */
static int started_by_desktop( void )
{
    char text[16];
    long fd = early_syscall3( 5 /* SYS_open */, (long)DESKTOP_PID_FILE, 0 /* O_RDONLY */, 0 ), n, pid = 0, i;

    if (fd < 0) return 0;
    n = early_syscall3( 3 /* SYS_read */, fd, (long)text, sizeof(text) - 1 );
    early_syscall3( 6 /* SYS_close */, fd, 0, 0 );
    for (i = 0; i < n && text[i] >= '0' && text[i] <= '9'; i++) pid = pid * 10 + text[i] - '0';
    return pid > 0 && early_syscall3( 39 /* SYS_getppid */, 0, 0, 0 ) == pid;
}

static void write_desktop_pid( void )
{
    FILE *f = fopen( DESKTOP_PID_FILE, "w" );

    if (!f) return;
    fprintf( f, "%d\n", (int)getpid() );
    fclose( f );
}

/* The C heap of a program process comes from direct memory. The system's flexible memory is one
 * pool shared by every process of the app, and the PS4 itself takes from it: the desktop's 256 MiB
 * and 128 MiB for each program used it all up (v00.40: winedevice.exe, the 4th, got nothing and
 * hung, and the PS4 froze; v00.32's explorer.exe the same). The desktop keeps its heap there.
 * Called while the system libraries start, before _start, main and any allocation: no environment
 * yet, so the desktop's lock is what tells. (v00.33-v00.41 never ran this: ps4_kit.h declared it
 * weak, and the linker kept the kit's empty one.) The log tag shows in boot-trace.log which one ran. */
void ps4_kit_configure_heap( ps4_kit_heap_options *options )
{
    if (!is_program_process())
    {
        /* the desktop's heap in direct memory too (the app's, which it is): the system's flexible
         * memory is what the PS4 itself and the processes outside the 7 live on (v00.52: with this
         * heap's 256 MiB there and four of them outside, a new local process could not get its C
         * library's memory, "Failed to create the system memory", and the PS4 closed the app) */
        options->log_tag = "desktop";
        options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
        options->direct_max = 384u << 20;
        options->system_pool_max = 128u << 20;  /* (only if direct memory failed) */
        return;
    }
    if (started_by_desktop())
    {
        /* outside the system's processes of the app: none of the app's direct memory (what the
         * PS4's suspend and close of the app tear down, v00.42) and none of its system pool;
         * regular flexible memory, from one pool all of them share with the PS4 (v00.51: 96 MiB
         * heaps took it all by the third): 16 MiB, and 16 MiB more at a time as the heap fills,
         * 3 times at most */
        ps4_outside_process = 1;
        options->log_tag = "outside";
        options->regular_only = 1;
        options->regular_max = 16u << 20;
        options->max_overflow_heaps = 3;
        return;
    }
    options->log_tag = "program";
    options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    /* 64 MiB, and 64 MiB more at a time as it fills (8 times at most): the C library's heap of a Wine
     * process holds Wine's own Unix-side data, a few dozen MiB (a game's shader compiles more), while
     * Windows' memory is the memory layer's; a direct mapping takes its whole size at once, used or
     * not, from the direct memory all the app's processes share (v00.58: 256 MiB each in six local
     * processes, and Steam's web helper ran out) */
    options->direct_max = 64u << 20;
    options->overflow_direct_max = 64u << 20;
    options->max_overflow_heaps = 8;
    options->overflow_direct_reserve = 256u << 20;  /* (what the memory layer keeps for the app too) */
    /* (the automatic arenas of threads that wait for the heap's lock come out of the main heap: a few small ones) */
    options->auto_arena_size = 2u << 20;
    options->max_auto_arenas = 6;
    /* never the system pool: what the PS4 itself and the processes outside the 7 live on, and when it
     * runs out the PS4 closes the whole app ("Failed to create the system memory": v00.53, and v00.59,
     * whose 64 MiB direct heaps the kit never tried, every program taking 128 MiB of the pool instead);
     * a program with no heap fails alone. Never regular flexible memory either (a forked child reads a
     * nonsense amount of it free, and tried 512 GiB, 256 GiB, ...). */
    options->system_pool_max = 0;
    options->regular_reserve = (size_t)-1 - (32u << 20);
}

#ifndef WINEPS4_VERSION
#define WINEPS4_VERSION "00.00"
#endif

#define DATA_DIR   "/data/WinePS4"
#define PREFIX_DIR DATA_DIR "/prefix"

PS4_DIAG_DATA_DIR( DATA_DIR );

extern void __wine_main( int argc, char *argv[] );
extern void ps4_dirs_reset( void );
extern void ps4_thread_register( void );
extern void ps4_start_hang_watch( void );
extern void ps4_kit_set_min_thread_stack( size_t bytes );
extern void ps4_process_exit_now( int status ) __attribute__((noreturn));
/* the kernel's view of the app's threads, and the desktop's heartbeat for the programs (port/ps4_kstate.c) */
extern void ps4_heartbeat_open( void );
extern void ps4_heartbeat_beat( unsigned int beats );
extern void ps4_heartbeat_requests( unsigned int requests );
extern void ps4_heartbeat_child( int pid, const char *image );
extern void ps4_kstate_prepare( void );
extern void ps4_kstate_report( const char *reason );
extern void (*ps4_kstate_log)( const char *line );
extern void ps4_thread_name( const char *name );
extern void ps4_memory_guard_start( void );

/* ntdll starts every thread on a 1 MiB stack of its own (kernel_stack_size, pthread_attr_setstack):
 * ps4-kit's 2 MiB minimum would grow it past its end */
#define WINE_KERNEL_STACK_SIZE 0x100000

static void logf_( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
static void logf_( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
}

static int log_fd = -1;
static struct timespec app_start;
/* the freeze report's (below): the log thread's and the desktop's loop's counts, what the log thread
 * is doing (0 reading the pipe, 1 writing wine.log, 2 giving the desktop its text) and its pipe */
static unsigned int output_beats, desktop_beats;
static volatile int output_state;
static int output_read_fd = -1;

/* seconds since the app started */
static double app_seconds( void )
{
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );
    return (now.tv_sec - app_start.tv_sec) + (now.tv_nsec - app_start.tv_nsec) / 1e9;
}

/* stdout and stderr through a pipe: into wine.log, each line with the seconds since the app started
 * (what each start waits for), and the desktop's Terminal as they are */
static void *output_thread( void *arg )
{
    static char buffer[4096], out[4096 * 16];  /* (room for a time on every character) */
    ssize_t n;
    int fd = (int)(long)arg, line_start = 1;

    ps4_thread_register();  /* (its stack in the freeze report) */
    ps4_thread_name( "log" );
    while ((n = read( fd, buffer, sizeof(buffer) )) > 0)
    {
        output_state = 1;
        if (log_fd >= 0)
        {
            const char *p = buffer, *end = buffer + n;
            size_t len = 0;

            while (p < end)
            {
                const char *newline = memchr( p, '\n', end - p ), *next = newline ? newline + 1 : end;

                if (line_start) len += snprintf( out + len, sizeof(out) - len, "[%7.2f] ", app_seconds() );
                memcpy( out + len, p, next - p );
                len += next - p;
                line_start = newline != NULL;
                p = next;
            }
            write( log_fd, out, len );
        }
        output_state = 2;
        ps4_shell_output( buffer, n );
        output_state = 0;
        __atomic_add_fetch( &output_beats, 1, __ATOMIC_RELAXED );
    }
    return NULL;
}

/* a line straight into wine.log, not through the programs' pipe: the desktop's loop and the freeze
 * report, which must never wait for it */
void ps4_log_line( const char *text )
{
    char line[1024];
    int len;

    if (log_fd < 0) return;
    len = snprintf( line, sizeof(line) - 1, "[%7.2f] %s", app_seconds(), text );
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    if (len > 0 && line[len - 1] != '\n') line[len++] = '\n';
    write( log_fd, line, len );
}

/* the desktop's loop, each time round (the freeze report) */
void ps4_desktop_beat( void )
{
    ps4_heartbeat_beat( __atomic_add_fetch( &desktop_beats, 1, __ATOMIC_RELAXED ) );  /* (and the programs') */
}

static void open_log( void )
{
    int fds[2];
    pthread_t thread;

    clock_gettime( CLOCK_MONOTONIC, &app_start );
    mkdir( DATA_DIR, 0777 );
    log_fd = open( DATA_DIR "/wine.log", O_WRONLY | O_CREAT | O_TRUNC, 0666 );
    if (log_fd < 0) return;
    if (pipe( fds ) || pthread_create( &thread, NULL, output_thread, (void *)(long)fds[0] ))
    {
        dup2( log_fd, 1 );  /* the log alone */
        dup2( log_fd, 2 );
    }
    else
    {
        pthread_detach( thread );
        /* never waited on: every program process writes into it (they have it as 1 and 2), and a pipe
         * nobody empties stops all of them and the desktop at once, wineserver among them (its log
         * lines); v00.61 froze that way, all quiet at the same second. A line that does not fit is lost. */
        fcntl( fds[1], F_SETFL, fcntl( fds[1], F_GETFL ) | O_NONBLOCK );
        output_read_fd = fds[0];
        dup2( fds[1], 1 );
        dup2( fds[1], 2 );
        close( fds[1] );
    }
    setvbuf( stdout, NULL, _IONBF, 0 );
    setvbuf( stderr, NULL, _IONBF, 0 );
}

/* KEY=VALUE lines; '#' starts a comment */
static void load_env_file( void )
{
    char line[1024];
    FILE *f = fopen( DATA_DIR "/env.txt", "r" );

    if (!f) return;
    while (fgets( line, sizeof(line), f ))
    {
        char *eq, *end = line + strlen( line );
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!line[0] || line[0] == '#' || !(eq = strchr( line, '=' ))) continue;
        *eq = 0;
        setenv( line, eq + 1, 1 );
        logf_( "wineps4: env %s=%s\n", line, eq + 1 );
    }
    fclose( f );
}

/* The console's name for Windows (port/ps4_net.c): its System Name (Settings > System > System
 * Information) as a computer name: letters, digits and '-', 15 at most (Windows' NetBIOS names) */
extern int sceSystemServiceParamGetString( int id, char *buffer, size_t size );

static void set_computer_name( void )
{
    char name[65] = "", out[16];
    size_t i, n = 0;

    if (sceSystemServiceParamGetString( 6 /* SYSTEM_NAME */, name, sizeof(name) )) name[0] = 0;
    for (i = 0; name[i] && n < sizeof(out) - 1; i++)
    {
        const char c = name[i];

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) out[n++] = c;
        else if (n && out[n - 1] != '-') out[n++] = '-';
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = 0;
    setenv( "WINEPS4_HOSTNAME", n ? out : "PS4", 0 );  /* (env.txt may set it) */
    logf_( "wineps4: computer name %s\n", getenv( "WINEPS4_HOSTNAME" ) );
}

static unsigned long long copied_bytes;
static unsigned int copied_files;

static int copy_file( const char *from, const char *to, mode_t mode )
{
    static char buffer[1 << 20];
    int in, out, ok = 1;
    ssize_t n;

    if ((in = open( from, O_RDONLY )) < 0) return 0;
    /* a copy from an earlier version may be read-only (the package's files are): replaced, not
     * written into (v00.14 could not update the prefix); the copy is the user's to change */
    unlink( to );
    (void)mode;
    if ((out = open( to, O_WRONLY | O_CREAT | O_TRUNC, 0666 )) < 0)
    {
        close( in );
        return 0;
    }
    while ((n = read( in, buffer, sizeof(buffer) )) > 0)
    {
        if (write( out, buffer, n ) != n) { ok = 0; break; }
        copied_bytes += n;
    }
    if (n < 0) ok = 0;
    close( in );
    close( out );
    copied_files++;
    return ok;
}

static int copy_tree( const char *from, const char *to )
{
    DIR *dir;
    struct dirent *entry;
    int ok = 1;

    mkdir( to, 0777 );
    if (!(dir = opendir( from ))) return 0;
    while ((entry = readdir( dir )))
    {
        char src[1024], dst[1024];
        struct stat st;

        if (!strcmp( entry->d_name, "." ) || !strcmp( entry->d_name, ".." )) continue;
        snprintf( src, sizeof(src), "%s/%s", from, entry->d_name );
        snprintf( dst, sizeof(dst), "%s/%s", to, entry->d_name );
        if (stat( src, &st )) continue;
        if (S_ISDIR( st.st_mode )) ok &= copy_tree( src, dst );
        else if (!copy_file( src, dst, st.st_mode ))
        {
            logf_( "wineps4: could not copy %s: %s\n", src, strerror( errno ));
            ok = 0;
        }
    }
    closedir( dir );
    return ok;
}

/* system32's and syswow64's copies of the builtin DLLs are packaged once, in Wine's lib folders: the
 * package lists them as system32/<name> and syswow64/<name> (app/package.sh) and they are copied
 * from there (64-bit DLLs from x86_64-windows, 32-bit ones from i386-windows). */
static int copy_system32_from_lib( void )
{
    char line[256];
    int ok = 1;
    FILE *list = fopen( WINE_APP_DIR "/system32-from-lib.txt", "r" );

    if (!list) return 0;
    while (fgets( line, sizeof(line), list ))
    {
        char src[512], dst[512];
        const char *name = line, *lib = "x86_64-windows", *folder = "system32";

        line[strcspn( line, "\r\n" )] = 0;
        if (!line[0]) continue;
        if (!strncmp( line, "syswow64/", 9 )) name = line + 9, lib = "i386-windows", folder = "syswow64";
        else if (!strncmp( line, "system32/", 9 )) name = line + 9;
        snprintf( src, sizeof(src), WINE_APP_DIR "/lib/wine/%s/%s", lib, name );
        snprintf( dst, sizeof(dst), PREFIX_DIR "/drive_c/windows/%s/%s", folder, name );
        if (!copy_file( src, dst, 0644 ))
        {
            logf_( "wineps4: could not copy %s: %s\n", src, strerror( errno ));
            ok = 0;
        }
    }
    fclose( list );
    return ok;
}

/* Which prefix the console has: written once the copy is whole. A package with a newer one (v00.14:
 * the 32-bit side, syswow64) copies it over the old one; files the user added stay. */
#define PREFIX_VERSION "4 driver"
#define PREFIX_MARK    PREFIX_DIR "/.wineps4-prefix"

static int prefix_current( void )
{
    char line[64] = "";
    FILE *f = fopen( PREFIX_MARK, "r" );

    if (!f) return 0;
    if (!fgets( line, sizeof(line), f )) line[0] = 0;
    fclose( f );
    line[strcspn( line, "\r\n" )] = 0;
    return !strcmp( line, PREFIX_VERSION );
}

/* The prefix made by the PC's Wine build, without dosdevices/ (symbolic links cannot be packaged;
 * ntdll creates c: and z: when the folder is missing). system.reg is copied last, then the mark. */
static void setup_prefix( void )
{
    time_t start;
    FILE *mark;

    if (prefix_current()) return;
    logf_( "wineps4: copying the prefix (" PREFIX_VERSION ") from " WINE_APP_DIR "/prefix\n" );
    ps4_shell_status( "copying its files (first start of a version that has new ones, about a minute)" );
    start = time( NULL );
    mkdir( PREFIX_DIR, 0777 );
    if (!copy_tree( WINE_APP_DIR "/prefix/drive_c", PREFIX_DIR "/drive_c" ) || !copy_system32_from_lib() ||
        !copy_file( WINE_APP_DIR "/prefix/user.reg", PREFIX_DIR "/user.reg", 0644 ) ||
        !copy_file( WINE_APP_DIR "/prefix/userdef.reg", PREFIX_DIR "/userdef.reg", 0644 ) ||
        !copy_file( WINE_APP_DIR "/prefix/.update-timestamp", PREFIX_DIR "/.update-timestamp", 0644 ) ||
        !copy_file( WINE_APP_DIR "/prefix/system.reg", PREFIX_DIR "/system.reg", 0644 ))
        logf_( "wineps4: the prefix copy had errors\n" );
    else if ((mark = fopen( PREFIX_MARK, "w" )))
    {
        fprintf( mark, "%s\n", PREFIX_VERSION );
        fclose( mark );
    }
    logf_( "wineps4: copied %u files, %llu MiB in %ld s\n", copied_files, copied_bytes >> 20,
           (long)(time( NULL ) - start) );
}

/* --- Programs: a process each -------------------------------------------------------------------
 * The desktop starts /app0/wineps4.bin for a program (WINEPS4_PROGRAM is in the desktop's
 * environment, so every process it starts has it); Wine's CreateProcess starts it too
 * (WINESERVERSOCKET). Either way it runs the program and ends; the desktop learns how from wait4. */

extern int ps4_program_process;
extern int ps4_wineserver_stopped;
extern void ps4_start_wineserver_listening( void );

/* The lifeline: a pipe whose write end only the desktop holds (close-on-exec), its read end
 * inherited by every process started from it, and theirs (WINEPS4_LIFELINE = its number). Nothing is
 * ever written: the read returns when the desktop has ended, however it ended (closed from the PS4
 * menu, crashed), and the process ends at once. Left running, a program kept its direct memory and
 * its CPU time after the app was closed (a stuck wineboot spun on a core: a slow PS4, then a kernel
 * panic). */
static void make_lifeline( void )
{
    char value[16];
    int fds[2];

    if (pipe( fds ))
    {
        logf_( "wineps4: no lifeline pipe: %s\n", strerror( errno ) );
        return;
    }
    fcntl( fds[1], F_SETFD, FD_CLOEXEC );
    snprintf( value, sizeof(value), "%d", fds[0] );
    setenv( "WINEPS4_LIFELINE", value, 1 );
}

static void *lifeline_thread( void *arg )
{
    const int fd = (int)(long)arg;
    sigset_t set;
    char c;

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    while (read( fd, &c, 1 ) == -1 && errno == EINTR) ;
    ps4_process_exit_now( 1 );  /* (nothing that may lock: the other threads may hold anything) */
}

static void watch_lifeline( void )
{
    const char *value = getenv( "WINEPS4_LIFELINE" );
    pthread_attr_t attr;
    pthread_t thread;
    int fd;

    if (!value || (fd = atoi( value )) <= 2 || fcntl( fd, F_GETFD ) == -1)
    {
        logf_( "wineps4: process %d: no lifeline (%s)\n", (int)getpid(), value ? value : "not set" );
        return;
    }
    fcntl( fd, F_SETFD, 0 );  /* (kept across exec: the processes it starts watch it too) */
    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 64 << 10 );
    if (pthread_create( &thread, &attr, lifeline_thread, (void *)(long)fd ))
        logf_( "wineps4: process %d: lifeline thread did not start\n", (int)getpid() );
    else pthread_detach( thread );
    pthread_attr_destroy( &attr );
}

static volatile int wine_ready;  /* the prefix is in place and wineserver listens */
static volatile int wine_booted; /* wineboot has run (services.exe started), or gave up */
static volatile int desktop_up;  /* explorer.exe has made the Windows desktop, ended, or gave up */
static volatile int boot_pid = -1, explorer_pid = -1;

extern pthread_mutex_t ps4_spawn_lock;
extern int ps4_spawn_broker_start( void );
extern int ps4_local_spawn( char *const argv[], char *const envp[], int fd0, int fd1, int fd2, int server_fd );
extern int ps4_local_kill_all( void );
extern void ps4_local_kill( int pid );
extern int ps4_local_child( int argc, char **argv );
extern int ps4_local_child_setup( int outside, int *argc_out, char ***argv_out );
extern int ps4_outside_child( int pid );
extern long ps4_budget_id( void );

static int local_child;  /* this program process is a local process (the PS4 started it) */

/* the desktop's children that have not been collected yet (the reaper's) */
#define MAX_CHILDREN 256
static pthread_mutex_t children_lock = PTHREAD_MUTEX_INITIALIZER;
static int children[MAX_CHILDREN], child_count;

/* (image: its program, for the kernel's view of the app's threads) */
void ps4_child_started( int pid, const char *image )
{
    pthread_mutex_lock( &children_lock );
    if (child_count < MAX_CHILDREN) children[child_count++] = pid;
    ps4_heartbeat_child( pid, image ? image : "" );
    pthread_mutex_unlock( &children_lock );
}

static void child_ended( int pid )
{
    int i;

    pthread_mutex_lock( &children_lock );
    for (i = 0; i < child_count; i++)
        if (children[i] == pid)
        {
            children[i] = children[--child_count];
            break;
        }
    ps4_heartbeat_child( pid, NULL );
    pthread_mutex_unlock( &children_lock );
}

static int children_left( void )
{
    int n;

    pthread_mutex_lock( &children_lock );
    n = child_count;
    pthread_mutex_unlock( &children_lock );
    return n;
}

int ps4_wine_ready( void ) { return wine_ready && wine_booted && desktop_up; }

/* a program process has ended (the reaper's wait, or a local process's socket) */
static void process_gone( int pid, int code )
{
    child_ended( pid );
    if (pid == boot_pid) wine_booted = 1;
    else if (pid == explorer_pid) explorer_pid = -1;  /* (a program's first window starts another) */
    else ps4_shell_program_exited( pid, code );
}

/* a local process has ended, with its exit code (-1: it ended without telling one) */
void ps4_process_ended( int pid, int code )
{
    if (code == -1) logf_( "wineps4: process %d ended (no exit code)\n", pid );
    else logf_( "wineps4: process %d ended (exit code %d)\n", pid, code );
    process_gone( pid, code );
}

/* Starts /app0/wineps4.bin for args (a Unix path or Windows command line, then its arguments), with
 * extra_env added to the environment when it isn't NULL: its pid, or -1. A local process of the app;
 * where the PS4 refuses that, forked (the child only execs: the desktop has threads, nothing that
 * may lock after fork). */
static int launch( char *const args[], const char *extra_env )
{
    extern char **environ;
    char *argv[66], **envp = environ;
    int argc = 0, pid, count = 0, local;

    argv[argc++] = (char *)"/app0/wineps4.bin";
    while (args[argc - 1] && argc < 65) argv[argc] = args[argc - 1], argc++;
    argv[argc] = NULL;
    if (!wine_ready)
    {
        errno = EAGAIN;
        return -1;
    }
    if (extra_env)
    {
        while (environ[count]) count++;
        if (!(envp = malloc( (count + 2) * sizeof(*envp) ))) return -1;
        memcpy( envp, environ, count * sizeof(*envp) );
        envp[count] = (char *)extra_env;
        envp[count + 1] = NULL;
    }
    pthread_mutex_lock( &ps4_spawn_lock );
    local = ps4_local_spawn( argv, envp, -1, 1, 2, -1 );
    if (local != -ENOSYS) pid = local < 0 ? -1 : local;
    else if ((pid = fork()) == 0)
    {
        execve( argv[0], argv, envp );
        _exit( 127 );  /* (the kernel's exit: not ps4-kit's, which would close the app) */
    }
    else if (pid > 0) ps4_child_started( pid, args[0] );
    pthread_mutex_unlock( &ps4_spawn_lock );
    if (envp != environ) free( envp );
    if (local < 0 && local != -ENOSYS) errno = -local;
    if (pid > 0)
        logf_( "wineps4: started %s (%s %d)\n", args[0], local == -ENOSYS ? "forked process" :
               ps4_outside_child( pid ) ? "outside the PS4's 7, process" : "local process", pid );
    return pid;
}

int ps4_launch_program( char *const args[] )
{
    return launch( args, NULL );
}

/* Before the desktop ends or restarts: the wineserver kills every Windows process it knows, the
 * system every local process, the desktop every child it has left (the ones that never got to the
 * wineserver too), and this waits (3 s at most) for all of them to be gone */
void ps4_end_programs( void )
{
    extern int ps4_ended_count, ps4_end_done;
    const struct ps4_input_record record = { PS4_INPUT_END_PROGRAMS };
    int i, waited, total, locals;

    if (!wine_ready) return;
    if (__atomic_load_n( &ps4_wineserver_stopped, __ATOMIC_SEQ_CST ))
        logf_( "wineps4: wineserver has stopped\n" );
    else if (!ps4_server_input( &record ))
        logf_( "wineps4: could not ask wineserver to end the programs\n" );
    else
    {
        for (waited = 0; waited < 100 && !__atomic_load_n( &ps4_end_done, __ATOMIC_ACQUIRE ); waited++) usleep( 10000 );
        if (!ps4_end_done) logf_( "wineps4: wineserver did not end the programs\n" );
        else logf_( "wineps4: wineserver ended %d processes\n", ps4_ended_count );
    }
    pthread_mutex_lock( &ps4_spawn_lock );  /* (none starts from here on) */
    locals = ps4_local_kill_all();
    pthread_mutex_lock( &children_lock );
    total = child_count;
    for (i = 0; i < child_count; i++) kill( children[i], SIGKILL );
    pthread_mutex_unlock( &children_lock );
    for (waited = 0; waited < 300 && children_left(); waited++) usleep( 10000 );
    if (children_left()) logf_( "wineps4: %d of %d processes still there after 3 s\n", children_left(), total );
    else logf_( "wineps4: all %d processes ended (%d local)\n", total, locals );
    usleep( 200000 );  /* (the log's thread writes it out) */
}

/* The prefix's .update-timestamp says "disable": wineboot then never redoes the prefix's setup
 * from wine.inf (the package's wine.inf has another date than the PC's, which made the prefix) */
static void disable_prefix_update( void )
{
    char line[16] = "";
    FILE *f = fopen( PREFIX_DIR "/.update-timestamp", "r" );

    if (f)
    {
        if (!fgets( line, sizeof(line), f )) line[0] = 0;
        fclose( f );
    }
    if (!strncmp( line, "disable", 7 )) return;
    if ((f = fopen( PREFIX_DIR "/.update-timestamp", "w" )))
    {
        fputs( "disable\n", f );
        fclose( f );
    }
}

/* What Windows starts by itself, trimmed. The PS4 runs 7 processes of an app at most (local
 * processes: ps4_spawn_desk.c), and every start of the app waited for each of these (about a second
 * a process):
 *  - services the prefix starts with Windows (Start 2) that start here only when something asks for
 *    them (3): Eventlog (an svchost.exe), PlugPlay (plugplay.exe) and NDIS (a network driver). They
 *    took 2 of the 7 (v00.43: a second Window test found no room for its explorer.exe). nsiproxy (the
 *    network tables: interfaces, TCP) was one of them until its Unix half was built (v00.52): it
 *    starts with Windows again, in MountMgr's winedevice.exe (their group); nsi.dll only opens it.
 *  - the root devices of drivers that aren't built in, winebus and winebth: services.exe starts the
 *    driver of every root device whatever its Start, a winedevice.exe each that failed (c0000135);
 *    without their Service value they are left alone.
 *  - winemenubuilder.exe, which wineboot runs from RunServices (menu entries for Linux desktops; it
 *    failed too, c0000135).
 * services.exe and MountMgr (the drives) stay. Done on the registry file before wineserver loads it,
 * every start (a new prefix copy has them again); written only when an edit applied. */
struct reg_edit
{
    const char *key;   /* as in the file: backslashes doubled */
    const char *from;  /* the start of one of the key's lines */
    const char *to;    /* what it starts with instead (as long), or NULL: the line goes */
    const char *name;  /* for the log */
};

static const struct reg_edit startup_edits[] =
{
    { "System\\\\ControlSet001\\\\Services\\\\Eventlog", "\"Start\"=dword:00000002", "\"Start\"=dword:00000003", "Eventlog" },
    { "System\\\\ControlSet001\\\\Services\\\\PlugPlay", "\"Start\"=dword:00000002", "\"Start\"=dword:00000003", "PlugPlay" },
    { "System\\\\ControlSet001\\\\Services\\\\NDIS", "\"Start\"=dword:00000002", "\"Start\"=dword:00000003", "NDIS" },
    { "System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBUS", "\"Service\"=\"winebus\"", NULL, "winebus" },
    { "System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBTH", "\"Service\"=\"winebth\"", NULL, "winebth" },
    { "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\RunServices", "\"winemenubuilder\"=", NULL, "winemenubuilder" },
};

/* what earlier versions trimmed, back as the prefix has it */
static const struct reg_edit startup_restore_edits[] =
{
    { "System\\\\ControlSet001\\\\Services\\\\nsiproxy", "\"Start\"=dword:00000003", "\"Start\"=dword:00000002", "nsiproxy" },
};

/* the same, for every key whose name starts with the key's: a program's services (their names carry
 * its version). Chrome's updater, left by an installer that did not finish, started 3-4 processes at
 * every start of Windows (v00.48/49): on demand now, as Chrome's installer asks for it. */
static const struct reg_edit startup_prefix_edits[] =
{
    { "System\\\\ControlSet001\\\\Services\\\\GoogleUpdater", "\"Start\"=dword:00000002", "\"Start\"=dword:00000003", "GoogleUpdater" },
};

/* one edit of the registry file's text (*size: its length): whether it applied */
static int apply_reg_edit( char *text, size_t *size, const struct reg_edit *edit )
{
    const size_t from_len = strlen( edit->from );
    char header[160], *line, *end;

    snprintf( header, sizeof(header), "\n[%s]", edit->key );
    if (!(line = strstr( text, header ))) return 0;
    line += strlen( header );
    if (!(end = strstr( line, "\n[" ))) end = text + *size;  /* (the key's lines end at the next key) */
    for (; (line = memchr( line, '\n', end - line )); line++)
    {
        if (strncmp( line + 1, edit->from, from_len )) continue;
        if (edit->to) memcpy( line + 1, edit->to, from_len );
        else  /* from its newline to the next one */
        {
            char *next = memchr( line + 1, '\n', text + *size - (line + 1) );
            const size_t gone = (next ? next : text + *size) - line;

            memmove( line, line + gone, text + *size - (line + gone) + 1 );
            *size -= gone;
        }
        return 1;
    }
    return 0;
}

/* an edit (its "to" as long as its "from") in every key starting with edit->key: how many */
static int apply_reg_edit_all( char *text, size_t size, const struct reg_edit *edit )
{
    const size_t from_len = strlen( edit->from );
    char header[160], *at = text, *line, *end;
    int count = 0;

    snprintf( header, sizeof(header), "\n[%s", edit->key );
    while ((at = strstr( at, header )))
    {
        line = at + strlen( header );
        if (!(end = strstr( line, "\n[" ))) end = text + size;
        for (; (line = memchr( line, '\n', end - line )); line++)
            if (!strncmp( line + 1, edit->from, from_len ))
            {
                memcpy( line + 1, edit->to, from_len );
                count++;
                break;
            }
        at = end;
    }
    return count;
}

static void trim_windows_startup( void )
{
    char *text, demand[128] = "", off[128] = "", back[128] = "";
    unsigned int i;
    size_t len;
    long size;
    FILE *f = fopen( PREFIX_DIR "/system.reg", "rb" );

    if (!f) return;
    fseek( f, 0, SEEK_END );
    size = ftell( f );
    fseek( f, 0, SEEK_SET );
    if (size <= 0 || !(text = malloc( size + 1 )))
    {
        fclose( f );
        return;
    }
    if (fread( text, 1, size, f ) != (size_t)size) size = 0;
    fclose( f );
    len = size > 0 ? size : 0;
    text[len] = 0;
    for (i = 0; len && i < sizeof(startup_edits) / sizeof(startup_edits[0]); i++)
        if (apply_reg_edit( text, &len, &startup_edits[i] ))
        {
            strcat( startup_edits[i].to ? demand : off, " " );
            strcat( startup_edits[i].to ? demand : off, startup_edits[i].name );
        }
    for (i = 0; len && i < sizeof(startup_prefix_edits) / sizeof(startup_prefix_edits[0]); i++)
        if (apply_reg_edit_all( text, len, &startup_prefix_edits[i] ) && strlen( demand ) < sizeof(demand) - 40)
        {
            strcat( demand, " " );
            strcat( demand, startup_prefix_edits[i].name );
        }
    for (i = 0; len && i < sizeof(startup_restore_edits) / sizeof(startup_restore_edits[0]); i++)
        if (apply_reg_edit( text, &len, &startup_restore_edits[i] ))
        {
            strcat( back, " " );
            strcat( back, startup_restore_edits[i].name );
        }
    if ((demand[0] || off[0] || back[0]) && (f = fopen( PREFIX_DIR "/system.reg.new", "wb" )))
    {
        const int ok = fwrite( text, 1, len, f ) == len;
        if (!fclose( f ) && ok && !rename( PREFIX_DIR "/system.reg.new", PREFIX_DIR "/system.reg" ))
        {
            if (demand[0]) logf_( "wineps4: Windows services that start only when asked for now:%s\n", demand );
            if (off[0]) logf_( "wineps4: no longer started with Windows:%s\n", off );
            if (back[0]) logf_( "wineps4: started with Windows again:%s\n", back );
        }
    }
    free( text );
}

/* user.reg's defaults, added before wineserver loads it: only the values a key lacks (the user may have
 * changed one).
 * - Font names Windows programs ask for, given the package's Liberation 2 fonts (metric-compatible with
 *   Arial, Times New Roman and Courier New; app/package.sh puts them beside Wine's own). DirectWrite and
 *   win32u both read HKCU\Software\Wine\Fonts\Replacements, and leave a name alone when a font has it.
 *   Before, Chromium (Steam's web helper) found none of its last resorts (Arial, Segoe UI, Microsoft Sans
 *   Serif, Times New Roman, Courier New...: Wine's Tahoma is not one of them) and drew no text (v00.55).
 * - Direct3D 8 to 11 from DXVK, on the PS4's GPU through Vulkan (install_dxvk puts its DLLs in the
 *   prefix; Settings > Diagnostics can give programs Wine's own again). Steam's processes keep Wine's:
 *   they only ask what the GPU is, and RADV would open it in each of them.
 * - Sound from wineps4.drv, which is also Wine's audio driver: mmdevapi tries the drivers named in
 *   HKCU\Software\Wine\Drivers, Audio ("pulse,alsa,oss,coreaudio" without it: none of them here, so
 *   v00.60's programs had no audio device at all). */
struct reg_default
{
    const char *key;    /* as in the file: backslashes doubled */
    const char *name;
    const char *value;
};

#define DLL_OVERRIDES "Software\\\\Wine\\\\DllOverrides"
#define STEAM_DLLS    "Software\\\\Wine\\\\AppDefaults\\\\steam.exe\\\\DllOverrides"
#define WEB_DLLS      "Software\\\\Wine\\\\AppDefaults\\\\steamwebhelper.exe\\\\DllOverrides"
#define FONT_NAMES    "Software\\\\Wine\\\\Fonts\\\\Replacements"
#define DRIVERS       "Software\\\\Wine\\\\Drivers"

static const struct reg_default user_defaults[] =
{
    { FONT_NAMES, "Arial", "Liberation Sans" },
    { FONT_NAMES, "Helvetica", "Liberation Sans" },
    { FONT_NAMES, "Segoe UI", "Liberation Sans" },
    { FONT_NAMES, "Microsoft Sans Serif", "Liberation Sans" },
    { FONT_NAMES, "Verdana", "Liberation Sans" },
    { FONT_NAMES, "Calibri", "Liberation Sans" },
    { FONT_NAMES, "Times New Roman", "Liberation Serif" },
    { FONT_NAMES, "Georgia", "Liberation Serif" },
    { FONT_NAMES, "Courier New", "Liberation Mono" },
    { FONT_NAMES, "Consolas", "Liberation Mono" },
    { FONT_NAMES, "Lucida Console", "Liberation Mono" },
    { DLL_OVERRIDES, "d3d8", "native,builtin" },
    { DLL_OVERRIDES, "d3d9", "native,builtin" },
    { DLL_OVERRIDES, "d3d10core", "native,builtin" },
    { DLL_OVERRIDES, "d3d11", "native,builtin" },
    { DLL_OVERRIDES, "dxgi", "native,builtin" },
    { STEAM_DLLS, "d3d9", "builtin" },
    { STEAM_DLLS, "d3d11", "builtin" },
    { STEAM_DLLS, "dxgi", "builtin" },
    { WEB_DLLS, "d3d9", "builtin" },
    { WEB_DLLS, "d3d11", "builtin" },
    { WEB_DLLS, "dxgi", "builtin" },
    { DRIVERS, "Audio", "ps4" },
};

/* the registry file's text with one value added to a key (made if missing), or the same text when the key
 * has the value already; *size follows */
static char *reg_add_value( char *text, size_t *size, const struct reg_default *def )
{
    char header[160], quoted[96], line[160], *key, *end, *body, *found, *out;
    size_t at, len;

    snprintf( header, sizeof(header), "\n[%s]", def->key );
    snprintf( quoted, sizeof(quoted), "\n\"%s\"=", def->name );
    snprintf( line, sizeof(line), "\"%s\"=\"%s\"\n", def->name, def->value );
    if ((key = strstr( text, header )))
    {
        if (!(end = strstr( key + 1, "\n[" ))) end = text + *size;  /* (the key's lines end at the next key) */
        if ((found = strstr( key, quoted )) && found < end) return text;
        body = strchr( key + 1, '\n' );  /* after its line, and its #time= line */
        while (body && body < end && body[1] == '#') body = strchr( body + 1, '\n' );
        at = body && body < end ? (size_t)(body + 1 - text) : (size_t)(end - text);
        len = strlen( line );
        if (!(out = malloc( *size + len + 1 ))) return text;
        memcpy( out, text, at );
        memcpy( out + at, line, len );
        memcpy( out + at + len, text + at, *size - at + 1 );
    }
    else
    {
        char block[400];

        len = snprintf( block, sizeof(block), "%s[%s] %lu\n%s", *size && text[*size - 1] == '\n' ? "\n" : "\n\n",
                        def->key, (unsigned long)time( NULL ), line );
        if (!(out = malloc( *size + len + 1 ))) return text;
        memcpy( out, text, *size );
        memcpy( out + *size, block, len + 1 );
    }
    free( text );
    *size += len;
    return out;
}

/* v00.58 wrote its keys with single backslashes, which Wine's registry reads as top-level keys named
 * "SoftwareWineDllOverrides" and the like (its DXVK overrides never applied): those go. The text's length
 * after, how many went in *dropped. */
static size_t drop_v0058_keys( char *text, size_t len, unsigned int *dropped )
{
    static const char junk[] = "\n[SoftwareWine";
    char *at, *end;

    while ((at = strstr( text, junk )))
    {
        if (!(end = strstr( at + 1, "\n[" ))) end = text + len;
        memmove( at, end, text + len - end + 1 );
        len -= end - at;
        (*dropped)++;
    }
    return len;
}

static void add_user_defaults( void )
{
    char *text, added[512] = "";
    size_t len, before;
    unsigned int i, dropped = 0;
    long size;
    FILE *f = fopen( PREFIX_DIR "/user.reg", "rb" );

    if (!f) return;
    fseek( f, 0, SEEK_END );
    size = ftell( f );
    fseek( f, 0, SEEK_SET );
    if (size <= 0 || !(text = malloc( size + 1 )))
    {
        fclose( f );
        return;
    }
    if (fread( text, 1, size, f ) != (size_t)size) size = 0;
    fclose( f );
    len = size > 0 ? size : 0;
    text[len] = 0;
    if (len) len = drop_v0058_keys( text, len, &dropped );
    for (i = 0; len && i < sizeof(user_defaults) / sizeof(user_defaults[0]); i++)
    {
        before = len;
        text = reg_add_value( text, &len, &user_defaults[i] );
        if (len != before)
            snprintf( added + strlen( added ), sizeof(added) - strlen( added ), "%s%s", added[0] ? ", " : " ",
                      user_defaults[i].name );
    }
    if ((added[0] || dropped) && (f = fopen( PREFIX_DIR "/user.reg.new", "wb" )))
    {
        const int ok = fwrite( text, 1, len, f ) == len;
        if (!fclose( f ) && ok && !rename( PREFIX_DIR "/user.reg.new", PREFIX_DIR "/user.reg" ))
        {
            if (dropped) logf_( "wineps4: user.reg: %u keys v00.58 wrote misnamed removed\n", dropped );
            if (added[0]) logf_( "wineps4: user.reg defaults added:%s\n", added );
        }
    }
    free( text );
}

/* DXVK (Direct3D 8 to 11 on Vulkan), from the package (app/package.sh) into the prefix: its 64-bit DLLs
 * in system32, its 32-bit ones in syswow64, where Wine's own were; each when it isn't the same file
 * already (a new package's DXVK, or Wine's DLL a prefix copy put back). user.reg's DLL overrides make
 * Wine take them (add_user_defaults). */
static void install_dxvk( void )
{
    static const char *const dlls[] = { "d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll" };
    static const char *const sides[][2] = { { "x64", "system32" }, { "x32", "syswow64" } };
    unsigned int i, j, copied = 0, failed = 0;
    char from[256], to[256];
    struct stat a, b;

    for (i = 0; i < 2; i++)
        for (j = 0; j < sizeof(dlls) / sizeof(dlls[0]); j++)
        {
            snprintf( from, sizeof(from), WINE_APP_DIR "/dxvk/%s/%s", sides[i][0], dlls[j] );
            snprintf( to, sizeof(to), PREFIX_DIR "/drive_c/windows/%s/%s", sides[i][1], dlls[j] );
            if (stat( from, &a )) continue;
            if (!stat( to, &b ) && b.st_size == a.st_size) continue;
            if (copy_file( from, to, 0644 )) copied++;
            else failed++;
        }
    if (copied || failed) logf_( "wineps4: DXVK's DLLs into the prefix: %u copied, %u failed\n", copied, failed );
}

/* Wine's startup, once per app start: wineboot --init (this machine's registry keys, then
 * services.exe, which stays), and beside it the Windows desktop, explorer.exe /desktop, which stays
 * too (the wineserver no longer closes the desktop when its last program ends: server/winstation.c).
 * A program's first window waited for explorer.exe otherwise, and for the rpcss.exe it starts, every
 * time the last program had ended; the first one also for the font cache it makes (FreeType reads
 * every font). The programs wait for both (ps4_wine_ready): explorer.exe until its display driver
 * reaches the desktop (the desktop window is made), it ends, or 20 s passed. */
static void boot_windows( void )
{
    static char *const boot_args[] = { (char *)"C:\\windows\\system32\\wineboot.exe", (char *)"--init", NULL };
    static char *const desktop_args[] = { (char *)"C:\\windows\\system32\\explorer.exe", (char *)"/desktop", NULL };
    const double start = app_seconds();
    int pid;

    disable_prefix_update();
    boot_pid = launch( boot_args, "WINEPS4_BOOT=1" );
    if (boot_pid == -1) logf_( "wineps4: wineboot did not start: %s\n", strerror( errno ) );
    explorer_pid = pid = launch( desktop_args, NULL );
    if (pid == -1) logf_( "wineps4: explorer.exe did not start: %s\n", strerror( errno ) );
    while (!wine_booted && boot_pid != -1)
    {
        if (app_seconds() - start > 120)
        {
            logf_( "wineps4: wineboot still running after 120 s: ended; programs start anyway\n" );
            ps4_local_kill( boot_pid );
            break;
        }
        usleep( 20000 );
    }
    if (wine_booted) logf_( "wineps4: wineboot done in %.1f s\n", app_seconds() - start );
    wine_booted = 1;
    while (pid != -1 && explorer_pid == pid && !ps4_shell_display_connected( pid ) && app_seconds() - start < 20)
        usleep( 20000 );
    if (pid != -1)
    {
        if (explorer_pid != pid) logf_( "wineps4: explorer.exe ended before it made the desktop\n" );
        else if (!ps4_shell_display_connected( pid )) logf_( "wineps4: no Windows desktop after 20 s: programs start anyway\n" );
        else logf_( "wineps4: Windows desktop up in %.1f s\n", app_seconds() - start );
    }
    desktop_up = 1;
}

/* The Windows user's folders (C:\users\ps4): the prefix comes from the PC, where its user had
 * another name, so the console's had only what Wine makes on demand (AppData): no Downloads for
 * browsers to save to, no Desktop or Documents, no AppData\LocalLow (v00.46: cryptnet's revocation
 * cache failed in Steam) */
static void make_user_folders( void )
{
    static const char *const folders[] = {
        "AppData", "AppData/Local", "AppData/Local/Temp", "AppData/LocalLow", "AppData/Roaming",
        "Contacts", "Desktop", "Documents", "Downloads", "Favorites", "Links", "Music", "Pictures",
        "Saved Games", "Searches", "Videos",
    };
    const char *user = getenv( "USER" );
    char path[512];
    unsigned int i;

    snprintf( path, sizeof(path), PREFIX_DIR "/drive_c/users/%s", user ? user : "ps4" );
    mkdir( path, 0777 );
    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
    {
        snprintf( path, sizeof(path), PREFIX_DIR "/drive_c/users/%s/%s", user ? user : "ps4", folders[i] );
        mkdir( path, 0777 );
    }
}

/* Settings > Diagnostics > Freeze report (WINEPS4_FREEZE_REPORT=0: off). v00.61 froze 100 s into
 * Steam with nothing in wine.log to say where. The desktop's loop and the thread that writes the log
 * each move a count; when one stands still for 6 s (the log thread with output waiting in its pipe),
 * where each of the desktop's threads is goes into hang.log (wineserver's thread among them), and
 * wine.log says so, written straight: the pipe may be what stands. Once a stand, three at most.
 * Nothing here takes a lock or prints with printf (ps4-kit's stdio lock: v00.63's report stopped
 * part way). (wineserver alone may rightly wait long, with only Windows' own programs: its stand is
 * told where it matters, when it stops taking the desktop's input, server/ps4_input.inc.) */
#define FREEZE_SECONDS 6
#define PS4_FIONREAD 0x4004667fUL  /* FreeBSD's (the C library's headers have Linux's) */
extern unsigned int ps4_request_seq;  /* wineserver's requests so far (server/fd.c) */
extern void ps4_dump_threads_quiet( const char *reason );
extern volatile int ps4_windows_lock_holder;  /* (port/ps4_wine_port.c, set by shell/winwindows.cpp) */
extern volatile int ps4_server_where, ps4_server_req, ps4_server_req_pid;  /* (server/fd.c) */
extern const char *ps4_request_name( int req );  /* (server/trace.c) */
extern unsigned long long ps4_vm_direct_free( void );
int sceKernelAvailableFlexibleMemorySize( size_t *size );

/* where wineserver's thread is now: told by the server itself, as its thread may not answer the
 * dump's signal (v00.63's dump got no further than the thread before it) */
static void server_where( struct ps4_line *text )
{
    const int where = ps4_server_where;

    if (where == 1)
    {
        ps4_line_str( text, " (now in its request " );
        ps4_line_str( text, ps4_request_name( ps4_server_req ) );
        ps4_line_str( text, " from process " );
        ps4_line_num( text, ps4_server_req_pid );
        ps4_line_str( text, ")" );
    }
    else ps4_line_str( text, where == 0 ? " (now waiting for its programs)" : where == 2 ? " (now on a file's event)"
                                                                                    : " (now running its timers)" );
}

/* a line into wine.log with its time, built and written without printf or a lock */
static void freeze_log( struct ps4_line *line )
{
    struct ps4_line out;

    out.len = 0;
    ps4_line_str( &out, "[" );
    ps4_line_seconds( &out, app_seconds(), 4 );
    ps4_line_str( &out, "] " );
    line->text[line->len] = 0;
    ps4_line_str( &out, line->text );
    ps4_line_str( &out, "\n" );
    if (log_fd >= 0) write( log_fd, out.text, out.len );
}

/* the kernel report's lines for wine.log (port/ps4_kstate.c) */
static void kstate_log( const char *line )
{
    struct ps4_line l;

    l.len = 0;
    ps4_line_str( &l, line );
    freeze_log( &l );
}

/* The kernel's view of the app's threads at a freeze (port/ps4_kstate.c), on a thread of its own: it
 * and the signals' dump don't wait for each other (v00.64's dump stood part way, and nothing after
 * it was told). */
static int kstate_wake[2] = { -1, -1 };
static char kstate_reason[256];

static void *kstate_thread( void *arg )
{
    sigset_t set;
    char c;

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    ps4_thread_name( "kernel report" );
    ps4_kstate_prepare();  /* (its pages touched now: the freeze may be the memory's) */
    while (early_syscall3( 3 /* SYS_read */, kstate_wake[0], (long)&c, 1 ) == 1) ps4_kstate_report( kstate_reason );
    return NULL;
}

static void *freeze_watch( void *arg )
{
    unsigned int desktop = desktop_beats, output = output_beats, requests[FREEZE_SECONDS] = { 0 };
    int desktop_still = 0, output_still = 0, reports = 0, standing = 0, tick = 0;
    struct ps4_line text;
    size_t flexible = 0;
    pthread_t thread;
    sigset_t set;

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    ps4_thread_name( "freeze watch" );
    if (!pipe( kstate_wake ) && !pthread_create( &thread, NULL, kstate_thread, NULL )) pthread_detach( thread );
    for (;;)
    {
        int pending = 0;

        sleep( 1 );
        ps4_heartbeat_requests( ps4_request_seq );
        if (!desktop_beats) continue;  /* (the desktop's loop has not started yet: its screen, its wallpaper) */
        if (output_read_fd >= 0 && ioctl( output_read_fd, PS4_FIONREAD, &pending )) pending = 0;
        desktop_still = desktop_beats == desktop ? desktop_still + 1 : 0;
        output_still = output_beats == output && pending > 0 ? output_still + 1 : 0;
        desktop = desktop_beats;
        output = output_beats;
        requests[tick++ % FREEZE_SECONDS] = ps4_request_seq;  /* (the one 10 s ago is overwritten next) */
        if (desktop_still < FREEZE_SECONDS && output_still < FREEZE_SECONDS)
        {
            standing = 0;
            continue;
        }
        if (standing || reports == 3) continue;
        standing = 1;
        reports++;
        text.len = 0;
        ps4_line_str( &text, "wineps4: a freeze: the desktop's loop still for " );
        ps4_line_num( &text, desktop_still );
        ps4_line_str( &text, " s, the log thread for " );
        ps4_line_num( &text, output_still );
        ps4_line_str( &text, output_state == 1 ? " s (writing wine.log, " : output_state == 2 ? " s (giving the desktop its text, "
                                                                                      : " s (reading, " );
        ps4_line_num( &text, pending );
        ps4_line_str( &text, " bytes waiting for it), wineserver " );
        ps4_line_num( &text, ps4_request_seq - requests[tick % FREEZE_SECONDS] );
        ps4_line_str( &text, " requests in the last " );
        ps4_line_num( &text, FREEZE_SECONDS - 1 );
        ps4_line_str( &text, " s" );
        server_where( &text );
        ps4_line_str( &text, ", the desktop's heap " );
        ps4_line_num( &text, (long long)ps4_heap_in_use_mib( NULL ) );  /* (lock-free) */
        ps4_line_str( &text, " MiB in use; the windows' lock " );
        if (ps4_windows_lock_holder)
        {
            ps4_line_str( &text, "held by thread " );
            ps4_line_num( &text, ps4_windows_lock_holder );
        }
        else ps4_line_str( &text, "free" );
        ps4_line_str( &text, ": its threads go into /data/WinePS4/hang.log" );
        freeze_log( &text );
        text.text[text.len] = 0;
        /* the kernel's view first (its own thread), then the signals' */
        if (kstate_wake[1] >= 0)
        {
            struct ps4_line reason;

            reason.len = 0;
            ps4_line_str( &reason, "wineps4: a freeze: the desktop's loop still for " );
            ps4_line_num( &reason, desktop_still );
            ps4_line_str( &reason, " s" );
            reason.text[reason.len] = 0;
            memcpy( kstate_reason, reason.text, reason.len + 1 );
            early_syscall3( 4 /* SYS_write */, kstate_wake[1], (long)"", 1 );
        }
        ps4_dump_threads_quiet( text.text );
        /* the app's memory after the report, as asking the kernel may itself be what stands */
        text.len = 0;
        ps4_line_str( &text, "wineps4: the freeze's thread stacks are in hang.log; the app's memory: direct " );
        ps4_line_num( &text, (long long)(ps4_vm_direct_free() >> 20) );
        ps4_line_str( &text, " MiB free in one run, flexible " );
        ps4_line_num( &text, sceKernelAvailableFlexibleMemorySize( &flexible ) ? -1 : (long long)(flexible >> 20) );
        if (ps4_outside_pool() != ~0ull)
        {
            ps4_line_str( &text, " MiB, the pool outside the 7 " );
            ps4_line_num( &text, (long long)(ps4_outside_pool() >> 20) );
        }
        ps4_line_str( &text, " MiB" );
        freeze_log( &text );
    }
    return NULL;
}

static void start_freeze_watch( void )
{
    const char *setting = getenv( "WINEPS4_FREEZE_REPORT" );
    pthread_t thread;

    if (setting && !strcmp( setting, "0" )) return;
    if (!pthread_create( &thread, NULL, freeze_watch, NULL )) pthread_detach( thread );
}

/* the programs' ends, for the desktop */
static void *reaper_thread( void *arg )
{
    ps4_thread_register();  /* (the freeze report's) */
    ps4_thread_name( "reaper" );
    for (;;)
    {
        int status = 0;
        const pid_t pid = waitpid( -1, &status, 0 );
        if (pid > 0 && ps4_outside_child( pid )) continue;  /* (its socket told its end, with the whole exit code) */
        if (pid > 0)
        {
            const int code = WIFEXITED( status ) ? WEXITSTATUS( status ) : 128 + WTERMSIG( status );
            logf_( "wineps4: process %d ended (%s %d)\n", (int)pid, WIFEXITED( status ) ? "exit code" : "signal",
                   WIFEXITED( status ) ? code : WTERMSIG( status ) );
            process_gone( pid, code );
        }
        else sleep( 1 );  /* none running */
    }
    return NULL;
}

/* the program's arguments from launch.txt (removed: it runs once) or run.txt, into args; false: none */
static int read_program( const char *path, int remove_it, char **args, int max )
{
    char line[1024];
    int n = 0;
    FILE *f = fopen( path, "r" );

    if (!f) return 0;
    while (n < max - 1 && fgets( line, sizeof(line), f ))
    {
        char *end = line + strlen( line );
        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        if (line[0] && line[0] != '#') args[n++] = strdup( line );
    }
    fclose( f );
    if (remove_it) unlink( path );
    args[n] = NULL;
    return n > 0;
}

extern void ps4_files_watch( void );
extern void ps4_budget_log( const char *why );
extern void ps4_budget_probe( void );

/* the console's open files, every 5 s (port/ps4_wine_dirs.c; Settings > Diagnostics > Open files) */
static void *files_thread( void *arg )
{
    ps4_thread_register();
    ps4_thread_name( "files" );
    for (;;)
    {
        ps4_files_watch();
        sleep( 5 );
    }
    return NULL;
}

/* the desktop's Windows: the prefix (copied on a version's first start), wineserver, Windows' own
 * startup; then what launch.txt or run.txt asks for */
static void *setup_thread( void *arg )
{
    static char *args[64];
    pthread_t reaper, files;
    const char *files_log = getenv( "WINEPS4_FILES_LOG" );
    int waited;

    ps4_shell_status( "starting" );
    setup_prefix();
    make_user_folders();
    trim_windows_startup();
    add_user_defaults();
    install_dxvk();
    ps4_start_wineserver_listening();
    /* (its socket is there once the thread has opened it, before the registry loads: the first
     * program queues; a server that can't start ends at once) */
    for (waited = 0; waited < 20 && !__atomic_load_n( &ps4_wineserver_stopped, __ATOMIC_SEQ_CST ); waited++)
        usleep( 5000 );
    if (__atomic_load_n( &ps4_wineserver_stopped, __ATOMIC_SEQ_CST ))
    {
        ps4_shell_status( "did not start: wineserver stopped (see /data/WinePS4/wine.log)" );
        logf_( "wineps4: wineserver did not start\n" );
        return NULL;
    }
    if (!ps4_spawn_broker_start()) logf_( "wineps4: programs start their processes themselves\n" );
    {
        /* (raised to its most by wineserver as it started: every program's handles are open here
         * too; FreeBSD's number, the C library's headers have Linux's) */
        struct rlimit files;
        if (!getrlimit( 8 /* RLIMIT_NOFILE */, &files ))
            logf_( "wineps4: open files: %llu at once (the most allowed %llu)\n", (unsigned long long)files.rlim_cur,
                   (unsigned long long)files.rlim_max );
    }
    /* the PS4's budget for the app (its open files are the ones that ran out), and whether a process
     * of the app may leave it (Settings > Diagnostics > Budget log) */
    if (!files_log || strcmp( files_log, "0" ))
    {
        ps4_budget_log( "the app's, at start" );
        ps4_budget_probe();
    }
    wine_ready = 1;
    if (!pthread_create( &reaper, NULL, reaper_thread, NULL )) pthread_detach( reaper );
    if ((!files_log || strcmp( files_log, "0" )) && !pthread_create( &files, NULL, files_thread, NULL ))
        pthread_detach( files );
    boot_windows();
    ps4_shell_status( NULL );
    logf_( "wineps4: Windows is ready, %.1f s after the app started\n", app_seconds() );
    if (read_program( WINEPS4_LAUNCH_FILE, 1, args, 64 ) || read_program( DATA_DIR "/run.txt", 0, args, 64 ))
        ps4_shell_launch( args );
    return NULL;
}

/* A program process: Wine, on the main thread, for the program in argv[1] */
/* A program's GPU (Vulkan on RADV, Direct3D on DXVK): RADV keeps the shaders it compiles in the data
 * folder (DXVK's games compile thousands; a cache makes the next start quick), and Settings > Apps can
 * give programs Wine's own Direct3D again (WINEPS4_DXVK=0; the registry's overrides are DXVK's,
 * add_user_defaults) */
static void gpu_environment( void )
{
    const char *dxvk = getenv( "WINEPS4_DXVK" );

    mkdir( DATA_DIR "/shadercache", 0777 );
    setenv( "MESA_SHADER_CACHE_DIR", DATA_DIR "/shadercache", 0 );
    if (dxvk && !strcmp( dxvk, "0" )) setenv( "WINEDLLOVERRIDES", "d3d8,d3d9,d3d10core,d3d11,dxgi=b", 0 );
}

/* Chromium's browsers (Steam's web helper among them) are told of the app's free memory less 1 GiB
 * (ps4_wine_vm.c's ps4_vm_set_memory_margin) */
extern void ps4_vm_set_memory_margin( unsigned long long margin );

static void chromium_memory_margin( const char *image )
{
    static const char *const browsers[] = { "chrome.exe", "msedge.exe", "brave.exe", "vivaldi.exe", "opera.exe",
                                            "steamwebhelper.exe", "msedgewebview2.exe" };
    const char *name = image, *p;
    unsigned int i;

    for (p = image; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    for (i = 0; i < sizeof(browsers) / sizeof(browsers[0]); i++)
        if (!strcasecmp( name, browsers[i] ))
        {
            ps4_vm_set_memory_margin( 1ull << 30 );
            return;
        }
}

static int program_main( int argc, char **argv )
{
    ps4_program_process = 1;
    setvbuf( stdout, NULL, _IONBF, 0 );  /* (wine.log and the Terminal, through the desktop's pipe) */
    setvbuf( stderr, NULL, _IONBF, 0 );
    if (!local_child) watch_lifeline();  /* (a local process watches its socket to the desktop) */
    if (argc < 2)
    {
        logf_( "wineps4: process %d: no program\n", (int)getpid() );
        ps4_process_exit_now( 1 );
    }
    if (ps4_outside_process)
        logf_( "wineps4: process %d: %s (outside the PS4's 7, budget %ld, regular flexible memory only)\n",
               (int)getpid(), argv[1], ps4_budget_id() );
    else logf_( "wineps4: process %d: %s\n", (int)getpid(), argv[1] );
    ps4_thread_register();  /* for the hang dump; ntdll registers the threads it starts */
    ps4_start_hang_watch();
    gpu_environment();
    chromium_memory_margin( argv[1] );
    __wine_main( argc, argv );
    ps4_process_exit_now( 0 );  /* (returning from main would be ps4-kit's exit: the whole app) */
}

int main( int argc, char **argv )
{
    pthread_t thread;
    pthread_attr_t attr;
    int kind;

    ps4_kit_set_min_thread_stack( WINE_KERNEL_STACK_SIZE );
    if ((kind = ps4_local_child( argc, argv )))  /* the PS4 (or the desktop) started it: what to run comes from the desktop */
    {
        char **program_argv;
        int program_argc;

        if (!ps4_local_child_setup( kind == 2, &program_argc, &program_argv )) ps4_process_exit_now( 127 );
        local_child = 1;
        return program_main( program_argc, program_argv );
    }
    if (getenv( "WINESERVERSOCKET" ) || getenv( "WINEPS4_PROGRAM" )) return program_main( argc, argv );

    /* the desktop: holds its lock (taken in static init unless the data folder wasn't there yet) */
    if (desktop_lock < 0)
    {
        mkdir( DATA_DIR, 0777 );
        is_program_process();
    }
    write_desktop_pid();  /* (before any program starts: the ones it starts itself tell by it) */
    open_log();
    ps4_heartbeat_open();  /* (before any program starts: they watch the desktop by it) */
    ps4_kstate_log = kstate_log;
    ps4_dirs_reset();  /* (the processes' shared folder paths: none from the last session) */
    logf_( "wineps4: WinePS4 v" WINEPS4_VERSION " (Wine 11.0)\n" );
    if (desktop_lock < 0)
        logf_( "wineps4: %s could not be locked: programs will take the system's memory\n", DESKTOP_LOCK_FILE );
    setenv( "HOME", DATA_DIR, 1 );
    setenv( "USER", "ps4", 1 );
    setenv( "WINEPREFIX", PREFIX_DIR, 1 );
    setenv( "WINELOADERNOEXEC", "1", 1 );  /* no re-exec through a preloader */
    setenv( "WINEDLLOVERRIDES", "winemenubuilder.exe=d", 0 );
    /* 32-bit programs in their process (WoW64), not through a 32-bit Wine loader the PS4 doesn't have:
     * without it Wine hands them to start.exe */
    setenv( "WINEARCH", "wow64", 0 );
    load_env_file();
    /* (Wine's stubs Chromium and Steam call all the time: 4000 of v00.60's 6000 lines of wine.log said
     * nothing but that; Settings > Diagnostics > Wine debug messages gives them all back) */
    setenv( "WINEDEBUG", "fixme-manipulation,fixme-dwmapi,fixme-appbar,fixme-media,fixme-bcrypt", 0 );
    set_computer_name();
    make_lifeline();
    setenv( "WINEPS4_PROGRAM", "1", 1 );  /* for the processes it starts (ps4_launch_program) */

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 1 << 20 );
    if (pthread_create( &thread, &attr, setup_thread, NULL )) logf_( "wineps4: could not start the setup thread\n" );
    pthread_attr_destroy( &attr );
    ps4_thread_register();  /* (the desktop's loop: the freeze report's first thread) */
    ps4_thread_name( "desktop" );
    start_freeze_watch();
    ps4_memory_guard_start();  /* (port/ps4_spawn_desk.c: the app's flexible memory) */
    ps4_shell_run( NULL );  /* does not return */
    return 0;
}
