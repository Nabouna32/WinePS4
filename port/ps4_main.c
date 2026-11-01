/* WinePS4: the app's entry point - the desktop (shell/) on the main thread, and Wine's loader
 * (loader/main.c) on a thread of its own for the program picked there.
 *
 *  /data/WinePS4/wine.log   everything Wine writes to stdout/stderr, and the program's console output
 *                           (also shown in the desktop's Terminal)
 *  /data/WinePS4/env.txt    optional KEY=VALUE lines put in the environment first (WINEDEBUG=+server);
 *                           the desktop's Settings write its switches there
 *  /data/WinePS4/launch.txt the program to run now (one argument per line): the desktop writes it,
 *                           then restarts the app; removed once read
 *  /data/WinePS4/run.txt    optional, for testing: a program to run at every start, like launch.txt
 *  /data/WinePS4/prefix     the Wine prefix, copied from the package (/app0/wine/prefix) on first run
 *  /data/WinePS4/crash.log  ps4-kit's report of a crash (boot-trace.log: the start up to it)
 *  /data/WinePS4/hang.log   where Wine's threads are when the program has not finished after 20 s
 *                           (WINEPS4_HANG_DUMP=<seconds>, 0 = off) */
#include <dirent.h>
#include <pthread.h>
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

/* The C heap of a program process comes from direct memory: the system's flexible memory is one
 * pool for the whole app, and 256 MiB heaps for the desktop and two programs left none for the
 * explorer.exe they started, which never got going (v00.32 froze). The desktop keeps its heap
 * there (it forks: a forked child has flexible memory, not direct memory). Called before main and
 * any allocation: it only reads the environment. */
void ps4_kit_configure_heap( ps4_kit_heap_options *options )
{
    extern char **environ;
    char **env;

    for (env = environ; env && *env; env++)
        if (!strncmp( *env, "WINESERVERSOCKET=", 17 ) || !strncmp( *env, "WINEPS4_PROGRAM=", 16 ))
        {
            options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
            options->direct_max = 256u << 20;
            return;
        }
}

#ifndef WINEPS4_VERSION
#define WINEPS4_VERSION "00.00"
#endif

#define DATA_DIR   "/data/WinePS4"
#define PREFIX_DIR DATA_DIR "/prefix"

PS4_DIAG_DATA_DIR( DATA_DIR );

extern void __wine_main( int argc, char *argv[] );
extern void ps4_thread_register( void );
extern void ps4_start_hang_watch( void );
extern void ps4_kit_set_min_thread_stack( size_t bytes );

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

/* stdout and stderr through a pipe: into wine.log and the desktop's Terminal */
static void *output_thread( void *arg )
{
    char buffer[4096];
    ssize_t n;
    int fd = (int)(long)arg;

    while ((n = read( fd, buffer, sizeof(buffer) )) > 0)
    {
        if (log_fd >= 0) write( log_fd, buffer, n );
        ps4_shell_output( buffer, n );
    }
    return NULL;
}

static void open_log( void )
{
    int fds[2];
    pthread_t thread;

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
    ps4_shell_status( "setting up Windows (first start of this version, about a minute)" );
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

static volatile int wine_ready;  /* the prefix is in place and wineserver listens */
static volatile int wine_booted; /* wineboot has run (services.exe started), or gave up */
static volatile int boot_pid = -1;

int ps4_wine_ready( void ) { return wine_ready && wine_booted; }

/* Starts /app0/wineps4.bin for args (a Unix path or Windows command line, then its arguments), with
 * extra_env added to the environment when it isn't NULL: its pid, or -1. The child only execs (the
 * desktop has threads: nothing that may lock after fork). */
static int launch( char *const args[], const char *extra_env )
{
    extern char **environ;
    char *argv[66], **envp = environ;
    int argc = 0, pid, count = 0;

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
    pid = fork();
    if (pid == 0)
    {
        execve( argv[0], argv, envp );
        _exit( 127 );  /* (the kernel's exit: not ps4-kit's, which would close the app) */
    }
    if (envp != environ) free( envp );
    if (pid > 0) logf_( "wineps4: started %s (process %d)\n", args[0], pid );
    return pid;
}

int ps4_launch_program( char *const args[] )
{
    return launch( args, NULL );
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

/* Wine's startup, once per app start: wineboot --init (this machine's registry keys, then
 * services.exe, which stays). The programs wait for it (ps4_wine_ready). */
static void boot_windows( void )
{
    static char *const args[] = { (char *)"C:\\windows\\system32\\wineboot.exe", (char *)"--init", NULL };
    const time_t start = time( NULL );

    disable_prefix_update();
    ps4_shell_status( "starting Windows services" );
    boot_pid = launch( args, "WINEPS4_BOOT=1" );
    if (boot_pid == -1) logf_( "wineps4: wineboot did not start: %s\n", strerror( errno ) );
    while (!wine_booted && boot_pid != -1)
    {
        if (time( NULL ) - start > 120)
        {
            logf_( "wineps4: wineboot still running after 120 s; programs start anyway\n" );
            break;
        }
        usleep( 100000 );
    }
    if (wine_booted) logf_( "wineps4: wineboot done in %ld s\n", (long)(time( NULL ) - start) );
    wine_booted = 1;
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
            if (pid == boot_pid) wine_booted = 1;
            else ps4_shell_program_exited( pid, code );
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

/* the desktop's Windows: the prefix (copied on a version's first start), then wineserver; then what
 * launch.txt or run.txt asks for */
static void *setup_thread( void *arg )
{
    static char *args[64];
    pthread_t reaper;

    ps4_shell_status( "setting up Windows" );
    setup_prefix();
    ps4_start_wineserver_listening();
    /* (the socket appears in the server directory once the thread has opened it; a server that
     * can't start ends at once) */
    usleep( 500000 );
    if (__atomic_load_n( &ps4_wineserver_stopped, __ATOMIC_SEQ_CST ))
    {
        ps4_shell_status( "did not start: wineserver stopped (see /data/WinePS4/wine.log)" );
        logf_( "wineps4: wineserver did not start\n" );
        return NULL;
    }
    wine_ready = 1;
    if (!pthread_create( &reaper, NULL, reaper_thread, NULL )) pthread_detach( reaper );
    boot_windows();
    ps4_shell_status( NULL );
    logf_( "wineps4: Windows is ready\n" );
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
    if (argc < 2)
    {
        logf_( "wineps4: process %d: no program\n", (int)getpid() );
        return 1;
    }
    logf_( "wineps4: process %d: %s\n", (int)getpid(), argv[1] );
    ps4_thread_register();  /* for the hang dump; ntdll registers the threads it starts */
    ps4_start_hang_watch();
    __wine_main( argc, argv );
    return 0;
}

int main( int argc, char **argv )
{
    pthread_t thread;
    pthread_attr_t attr;

    ps4_kit_set_min_thread_stack( WINE_KERNEL_STACK_SIZE );
    if (getenv( "WINESERVERSOCKET" ) || getenv( "WINEPS4_PROGRAM" )) return program_main( argc, argv );

    /* the desktop */
    open_log();
    logf_( "wineps4: WinePS4 v" WINEPS4_VERSION " (Wine 11.0)\n" );
    setenv( "HOME", DATA_DIR, 1 );
    setenv( "USER", "ps4", 1 );
    setenv( "WINEPREFIX", PREFIX_DIR, 1 );
    setenv( "WINELOADERNOEXEC", "1", 1 );  /* no re-exec through a preloader */
    setenv( "WINEDLLOVERRIDES", "winemenubuilder.exe=d", 0 );
    /* 32-bit programs in their process (WoW64), not through a 32-bit Wine loader the PS4 doesn't have:
     * without it Wine hands them to start.exe */
    setenv( "WINEARCH", "wow64", 0 );
    load_env_file();
    setenv( "WINEPS4_PROGRAM", "1", 1 );  /* for the processes it starts (ps4_launch_program) */

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 1 << 20 );
    if (pthread_create( &thread, &attr, setup_thread, NULL )) logf_( "wineps4: could not start the setup thread\n" );
    pthread_attr_destroy( &attr );
    ps4_shell_run( NULL );  /* does not return */
    return 0;
}
