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
#include <time.h>
#include <unistd.h>

#include "ps4_kit_diag.h"
#include "../shell/shell.h"

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
#define PREFIX_VERSION "2 wow64"
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

static char *wine_argv[64];
static int wine_argc;

/* the program's arguments from launch.txt (removed: it runs once) or run.txt; false: none */
static int read_program( const char *path, int remove_it )
{
    char line[1024];
    FILE *f = fopen( path, "r" );

    if (!f) return 0;
    wine_argc = 0;
    wine_argv[wine_argc++] = strdup( "wine" );  /* writable, as a real argv: Wine edits the strings in place */
    while (wine_argc < 63 && fgets( line, sizeof(line), f ))
    {
        char *end = line + strlen( line );
        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        if (line[0] && line[0] != '#') wine_argv[wine_argc++] = strdup( line );
    }
    fclose( f );
    if (remove_it) unlink( path );
    wine_argv[wine_argc] = NULL;
    return wine_argc > 1;
}

/* the program's thread: the prefix (copied on the first run), then Wine */
static void *wine_thread( void *arg )
{
    ps4_shell_status( "setting up" );
    setup_prefix();
    ps4_shell_status( NULL );
    ps4_thread_register();  /* for the hang dump; ntdll registers the threads it starts */
    ps4_start_hang_watch();
    logf_( "wineps4: running %s\n", wine_argv[1] );
    __wine_main( wine_argc, wine_argv );
    return NULL;
}

int main( void )
{
    const char *program = NULL;

    ps4_kit_set_min_thread_stack( WINE_KERNEL_STACK_SIZE );
    open_log();
    logf_( "wineps4: WinePS4 v" WINEPS4_VERSION " (Wine 11.0)\n" );

    setenv( "HOME", DATA_DIR, 1 );
    setenv( "USER", "ps4", 1 );
    setenv( "WINEPREFIX", PREFIX_DIR, 1 );
    setenv( "WINELOADERNOEXEC", "1", 1 );  /* no re-exec through a preloader */
    setenv( "WINEDLLOVERRIDES", "winemenubuilder.exe=d", 0 );
    /* 32-bit programs in this process (WoW64), not through a 32-bit Wine loader the PS4 doesn't have:
     * without it Wine hands them to start.exe, which can't start another process */
    setenv( "WINEARCH", "wow64", 0 );
    load_env_file();

    if (read_program( WINEPS4_LAUNCH_FILE, 1 ) || read_program( DATA_DIR "/run.txt", 0 ))
    {
        pthread_attr_t attr;
        pthread_t thread;

        program = wine_argv[wine_argc - 1];
        pthread_attr_init( &attr );
        pthread_attr_setstacksize( &attr, 8 << 20 );
        if (pthread_create( &thread, &attr, wine_thread, NULL ))
        {
            logf_( "wineps4: could not start the program's thread\n" );
            program = NULL;
        }
        pthread_attr_destroy( &attr );
    }
    else logf_( "wineps4: no program to run: the desktop\n" );

    ps4_shell_run( program );  /* does not return */
    return 0;
}
