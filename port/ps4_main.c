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
 * with the app; Power > Exit ends them first (ps4_end_programs). Where the PS4 refuses that, they are
 * the desktop's forked children, which end themselves when the desktop is gone (the lifeline). */
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ps4_kit_diag.h"
#include "ps4_kit.h"
#include "../shell/shell.h"
#include "include/ps4_display.h"

#define DESKTOP_LOCK_FILE "/data/WinePS4/.desktop-lock"  /* locked by the desktop while it runs */

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
        options->log_tag = "desktop";
        return;
    }
    options->log_tag = "program";
    options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    options->direct_max = 256u << 20;
    /* 128 MiB of the system pool only when direct memory failed; never regular flexible memory (a
     * forked child reads a nonsense amount of it free, and tried 512 GiB, 256 GiB, ...) */
    options->system_pool_max = 128u << 20;
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

    while ((n = read( fd, buffer, sizeof(buffer) )) > 0)
    {
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
        ps4_shell_output( buffer, n );
    }
    return NULL;
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
extern int ps4_local_child_setup( int *argc_out, char ***argv_out );

static int local_child;  /* this program process is a local process (the PS4 started it) */

/* the desktop's children that have not been collected yet (the reaper's) */
#define MAX_CHILDREN 256
static pthread_mutex_t children_lock = PTHREAD_MUTEX_INITIALIZER;
static int children[MAX_CHILDREN], child_count;

void ps4_child_started( int pid )
{
    pthread_mutex_lock( &children_lock );
    if (child_count < MAX_CHILDREN) children[child_count++] = pid;
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
    else if (pid > 0) ps4_child_started( pid );
    pthread_mutex_unlock( &ps4_spawn_lock );
    if (envp != environ) free( envp );
    if (local < 0 && local != -ENOSYS) errno = -local;
    if (pid > 0) logf_( "wineps4: started %s (%s %d)\n", args[0], local == -ENOSYS ? "forked process" : "local process", pid );
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
 *    them (3): Eventlog (an svchost.exe), PlugPlay (plugplay.exe), NDIS and nsiproxy (the network
 *    drivers; nsiproxy.so isn't built yet). They took 2 of the 7 (v00.43: a second Window test found
 *    no room for its explorer.exe).
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
    { "System\\\\ControlSet001\\\\Services\\\\nsiproxy", "\"Start\"=dword:00000002", "\"Start\"=dword:00000003", "nsiproxy" },
    { "System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBUS", "\"Service\"=\"winebus\"", NULL, "winebus" },
    { "System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBTH", "\"Service\"=\"winebth\"", NULL, "winebth" },
    { "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\RunServices", "\"winemenubuilder\"=", NULL, "winemenubuilder" },
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

static void trim_windows_startup( void )
{
    char *text, demand[128] = "", off[128] = "";
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
    if ((demand[0] || off[0]) && (f = fopen( PREFIX_DIR "/system.reg.new", "wb" )))
    {
        const int ok = fwrite( text, 1, len, f ) == len;
        if (!fclose( f ) && ok && !rename( PREFIX_DIR "/system.reg.new", PREFIX_DIR "/system.reg" ))
        {
            if (demand[0]) logf_( "wineps4: Windows services that start only when asked for now:%s\n", demand );
            if (off[0]) logf_( "wineps4: no longer started with Windows:%s\n", off );
        }
    }
    free( text );
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

/* the programs' ends, for the desktop */
static void *reaper_thread( void *arg )
{
    for (;;)
    {
        int status = 0;
        const pid_t pid = waitpid( -1, &status, 0 );
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

/* the desktop's Windows: the prefix (copied on a version's first start), wineserver, Windows' own
 * startup; then what launch.txt or run.txt asks for */
static void *setup_thread( void *arg )
{
    static char *args[64];
    pthread_t reaper;
    int waited;

    ps4_shell_status( "starting" );
    setup_prefix();
    make_user_folders();
    trim_windows_startup();
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
    wine_ready = 1;
    if (!pthread_create( &reaper, NULL, reaper_thread, NULL )) pthread_detach( reaper );
    boot_windows();
    ps4_shell_status( NULL );
    logf_( "wineps4: Windows is ready, %.1f s after the app started\n", app_seconds() );
    if (read_program( WINEPS4_LAUNCH_FILE, 1, args, 64 ) || read_program( DATA_DIR "/run.txt", 0, args, 64 ))
        ps4_shell_launch( args );
    return NULL;
}

/* A program process: Wine, on the main thread, for the program in argv[1] */
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
    logf_( "wineps4: process %d: %s\n", (int)getpid(), argv[1] );
    ps4_thread_register();  /* for the hang dump; ntdll registers the threads it starts */
    ps4_start_hang_watch();
    __wine_main( argc, argv );
    ps4_process_exit_now( 0 );  /* (returning from main would be ps4-kit's exit: the whole app) */
}

int main( int argc, char **argv )
{
    pthread_t thread;
    pthread_attr_t attr;

    ps4_kit_set_min_thread_stack( WINE_KERNEL_STACK_SIZE );
    if (ps4_local_child( argc, argv ))  /* the PS4 started it: what to run comes from the desktop */
    {
        char **program_argv;
        int program_argc;

        if (!ps4_local_child_setup( &program_argc, &program_argv )) ps4_process_exit_now( 127 );
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
    open_log();
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
    set_computer_name();
    make_lifeline();
    setenv( "WINEPS4_PROGRAM", "1", 1 );  /* for the processes it starts (ps4_launch_program) */

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 1 << 20 );
    if (pthread_create( &thread, &attr, setup_thread, NULL )) logf_( "wineps4: could not start the setup thread\n" );
    pthread_attr_destroy( &attr );
    ps4_shell_run( NULL );  /* does not return */
    return 0;
}
