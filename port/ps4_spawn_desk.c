/* WinePS4: the desktop starts every program process.
 *
 * A process Wine starts (CreateProcess: ntdll's spawn_process) is not forked by the program that
 * asks for it: the program sends the command line, the environment and its descriptors (standard
 * input and output, the wineserver socket) to the desktop over /data/WinePS4/.spawn, and the
 * desktop has the PS4 start it as a local process of the app (below) and answers with its pid.
 * Started by the program, a process was left without a parent when that program ended first
 * (explorer.exe after window.exe, winemenubuilder after wineboot).
 *
 * Built with ps4_wine_compat.h: the kernel's (FreeBSD) msghdr, cmsghdr and sockaddr_un. */
#include "ps4_wine_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "../shell/shell.h"
#include "include/ps4_line.h"

#define SPAWN_SOCKET "/data/WinePS4/.spawn"
#define SPAWN_MAGIC  0x50533457  /* "W4SP" */
#define SPAWN_MAX    (1 << 20)   /* the strings, at most */

struct spawn_header
{
    uint32_t magic;
    uint32_t argc;       /* then argc strings: path and arguments */
    uint32_t envc;       /* then envc strings: the environment */
    uint32_t size;       /* bytes of all the strings */
    uint32_t fds;        /* descriptors sent with it, in this order: 1 = stdin, 2 = stdout, 4 = server */
    uint32_t pad;
};

extern pid_t ps4_spawn( const char *path, char **argv, char **envp, int fd0, int fd1 );
extern void ps4_section_probe( void );  /* (port/ps4_wine_vm.c) */
extern void ps4_child_started( int pid, const char *image );
extern void ps4_thread_name( const char *name );  /* (port/ps4_kstate.c) */
extern void ps4_touch_stack( size_t size );

/* held while the desktop forks: no child gets a descriptor received for another one */
pthread_mutex_t ps4_spawn_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_all( int fd, const void *data, size_t size )
{
    const char *p = data;
    while (size)
    {
        ssize_t n = write( fd, p, size );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n, size -= n;
    }
    return 1;
}

static int read_all( int fd, void *data, size_t size )
{
    char *p = data;
    while (size)
    {
        ssize_t n = read( fd, p, size );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n, size -= n;
    }
    return 1;
}

static void socket_address( struct sockaddr_un *addr )
{
    memset( addr, 0, sizeof(*addr) );
    addr->sun_len = sizeof(*addr);
    addr->sun_family = AF_UNIX;
    strcpy( addr->sun_path, SPAWN_SOCKET );
}

/* --- Local processes: the PS4's own processes of the app ----------------------------------------
 *
 * The desktop has the PS4 start each program (sceSystemServiceAddLocalProcess): /app0/wineps4.bin as
 * a local process of WinePS4, which the system suspends and ends together with the app. Forked by the
 * desktop instead (v00.20-v00.42), the programs were processes the system knew nothing of, using the
 * app's direct memory and GPU space: closing the app from the PS4 menu tore those down under them
 * (kernel log: "Fatal trap 12: page fault while in kernel mode"), then could not unmount the app's
 * files they still held ("still in use", retried for ever): the PS4 was stuck until turned off.
 *
 * The system hands the new process one socket (sceSystemServiceGetParentSocket) and nothing else: no
 * environment, no other descriptors. Over it the child says HELLO (its pid), the desktop sends the
 * arguments, the environment and the descriptors (standard input, output and error, the wineserver
 * socket), the child says READY, runs the program and says EXIT (its code) as it ends; the end of the
 * socket tells either side that the other is gone. When the PS4 refuses local processes, the desktop
 * forks the programs as before (and wine.log says so).
 *
 * Past the PS4's 7 (the most local processes an app has: 0x80aa0008 for the 8th, v00.43): processes
 * the desktop starts itself, outside the system's own of the app (port/ps4_wine_port.c's
 * ps4_spawn_outside), with the same hand-off over a socket of their own (descriptor 3). Windows'
 * background programs and helpers start there first (background_programs), so the 7 stay for the
 * programs and games; any program when the 7 run. Forked processes with the app's direct memory
 * brought the PS4 down when it suspended and closed the app (v00.30-00.42): these take regular
 * flexible memory only (port/ps4_main.c, ps4_kit_configure_heap; port/ps4_wine_vm.c), and end the
 * moment the desktop does (their socket's end). Settings > Apps turns them off (WINEPS4_OUTSIDE=0). */

#define LOCAL_ARG        "--wineps4-local"  /* the argument the PS4 starts a local process with */
#define OUTSIDE_ARG      "--wineps4-outside" /* the one the desktop starts a process outside the 7 with */
#define LOCAL_MAGIC      0x504c3457         /* "W4LP": the desktop's hand-off */
#define MSG_HELLO        0x4f4c4548         /* "HELO", the pid */
#define MSG_READY        0x59444552         /* "REDY" */
#define MSG_EXIT         0x54495845         /* "EXIT", the exit code (port/ps4_wine_port.c) */
#define MSG_STATS        0x54415453         /* "STAT", then struct local_stats: every second, for Task Manager */
#define MAX_PROCS        48                 /* local (SCE_LNC_UTIL_MAX_LOCAL_PROCESS_NUM: 16) and outside */
#define LOCAL_TIMEOUT_MS 15000

struct local_msg
{
    uint32_t magic;
    int32_t value;
};

struct local_stats
{
    uint64_t cpu_us;  /* the process's CPU time */
    uint64_t memory;  /* what its memory layer has backed (port/ps4_wine_vm.c) */
    uint64_t pool;    /* outside the 7: the memory all of those share, free (the kernel's figure) */
    uint64_t flexible, shared;  /* its flexible and shared mappings, every 5 s (port/ps4_kstate.c) */
};

/* A process outside the 7 starts only while the memory they share with the PS4 has room for it: 75
 * to 115 MiB each in the PS4's own count (v00.52), and the PS4 keeps 128 MiB of it (port/
 * ps4_wine_vm.c). v00.51 ran it out by the fourth (rpcss.exe and steamservice.exe did not start);
 * v00.52 took it to 13 MiB, and a new local process could not start its C library: the PS4 closed
 * the app. Without the room the program goes among the 7. */
#define OUTSIDE_MIN_POOL (256ull << 20)
/* the app's free direct memory (beyond what's kept for it) under which a new program is tried outside
 * the 7 first (ps4_wine_vm.c) */
#define LOW_DIRECT_MEMORY (384ull << 20)
extern unsigned long long ps4_vm_direct_free( void );
static uint64_t outside_pool = ~0ull;  /* the last figure one of them sent */

extern int ps4_outside_process;
int32_t sceKernelAvailableFlexibleMemorySize( size_t *size );

int sceSystemServiceAddLocalProcess( int app_id, const char *path, const char *const *argv, const void *param );
int sceSystemServiceKillLocalProcess( int app_id, int local_id );
int sceSystemServiceGetAppIdOfBigApp( void );
int sceSystemServiceGetParentSocket( void );

extern void ps4_process_ended( int pid, int code );
extern void ps4_process_exit_now( int status ) __attribute__((noreturn));
extern void ps4_shell_notice( const char *text );
extern pid_t ps4_spawn_outside( const char *path, char **argv, char **envp, int control, int leave_budget );
extern int ps4_control_fd;

static pthread_mutex_t locals_lock = PTHREAD_MUTEX_INITIALIZER;
static struct
{
    int local_id, pid, control, code;  /* local_id -1: a process outside the PS4's 7 */
    char image[260];             /* the program (argv[1]: a Windows or Unix path) */
    struct local_stats stats;    /* the last it sent */
} locals[MAX_PROCS];
static int local_count;
static int local_state;            /* 0: not tried yet, 1: the PS4 starts them, -1: refused */
static int outside_state;          /* processes outside the 7: 0 not tried yet, 1 they start, -1 they don't */
static int outside_budget = 1;     /* they leave the app's budget (0: their exec failed that way) */
static int app_id = -1;
static int wake[2] = { -1, -1 };   /* wakes the monitor when a process is added */
static pthread_once_t monitor_once = PTHREAD_ONCE_INIT;

/* the pids started outside the 7 (their sockets tell their ends: the reaper leaves them alone) */
static int outside_pids[64];
static unsigned int outside_next;

/* a message from fd within timeout_ms: 1, or 0 (nothing in time, or the socket's end) */
static int read_msg( int fd, struct local_msg *msg, int timeout_ms )
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ret;

    while ((ret = poll( &pfd, 1, timeout_ms )) == -1 && errno == EINTR) ;
    return ret == 1 && read_all( fd, msg, sizeof(*msg) );
}

/* a message on a local process's socket: its figures, its exit code, or its end */
static void control_event( int fd )
{
    struct local_msg msg;
    struct local_stats stats;
    unsigned long long memory;
    ssize_t got;
    int i, pid, code, local_id, have_stats = 0;

    while ((got = read( fd, &msg, sizeof(msg) )) == -1 && errno == EINTR) ;
    if (got > 0 && got < (ssize_t)sizeof(msg) && read_all( fd, (char *)&msg + got, sizeof(msg) - got )) got = sizeof(msg);
    if (got == sizeof(msg) && msg.magic == MSG_STATS) have_stats = read_all( fd, &stats, sizeof(stats) );
    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count && locals[i].control != fd; i++) ;
    if (i == local_count || got > 0)
    {
        if (i < local_count && got == sizeof(msg) && msg.magic == MSG_EXIT) locals[i].code = msg.value;
        if (i < local_count && have_stats)
        {
            locals[i].stats = stats;
            if (locals[i].local_id < 0 && stats.pool) outside_pool = stats.pool;
        }
        pthread_mutex_unlock( &locals_lock );
        return;
    }
    pid = locals[i].pid;
    code = locals[i].code;
    local_id = locals[i].local_id;
    memory = locals[i].stats.memory;
    close( fd );
    locals[i] = locals[--local_count];
    pthread_mutex_unlock( &locals_lock );
    if (local_id >= 0) sceSystemServiceKillLocalProcess( app_id, local_id );  /* (its entry, if the system still has one) */
    else
    {
        kill( pid, SIGKILL );  /* (a socket closed early: what is left of it) */
        outside_pool = ~0ull;  /* (its memory is free again: the next figure tells) */
        fprintf( stderr, "wineps4: process %d outside the 7 had backed %llu MiB of Wine's memory\n", pid, memory >> 20 );
    }
    ps4_process_ended( pid, code );
}

/* the desktop: every local process's socket, for its end */
static void *local_monitor( void *arg )
{
    struct pollfd fds[MAX_PROCS + 1];
    char buf[16];
    int n, i;

    ps4_thread_register();  /* (the freeze report's) */
    ps4_thread_name( "spawn monitor" );
    for (;;)
    {
        fds[0].fd = wake[0];
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        pthread_mutex_lock( &locals_lock );
        for (i = 0, n = 1; i < local_count; i++, n++)
        {
            fds[n].fd = locals[i].control;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
        }
        pthread_mutex_unlock( &locals_lock );
        if (poll( fds, n, -1 ) <= 0) continue;
        if (fds[0].revents) read( wake[0], buf, sizeof(buf) );
        for (i = 1; i < n; i++)
            if (fds[i].revents) control_event( fds[i].fd );
    }
    return NULL;
}

static void start_monitor( void )
{
    pthread_t thread;

    if (pipe( wake ) == -1) return;
    fcntl( wake[0], F_SETFD, FD_CLOEXEC );
    fcntl( wake[1], F_SETFD, FD_CLOEXEC );
    if (!pthread_create( &thread, NULL, local_monitor, NULL )) pthread_detach( thread );
}

static int skip_env( const char *var )  /* the child's own (the lifeline is the forked ones') */
{
    return !strncmp( var, "WINESERVERSOCKET=", 17 ) || !strncmp( var, "WINEPS4_LIFELINE=", 17 );
}

/* Windows' background programs and the helpers of programs: outside the PS4's 7 first */
static const char *const background_programs[] =
{
    /* Windows' services, and the tools installers and scripts run */
    "services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "rpcss.exe", "dllhost.exe",
    "msiexec.exe", "conhost.exe", "spoolsv.exe", "tabtip.exe", "werfault.exe", "wusa.exe",
    "cmd.exe", "rundll32.exe", "regsvr32.exe", "reg.exe", "sc.exe", "net.exe", "schtasks.exe",
    "taskkill.exe", "tasklist.exe", "timeout.exe", "ping.exe", "ipconfig.exe", "netsh.exe",
    "hostname.exe", "whoami.exe", "where.exe", "find.exe", "findstr.exe", "attrib.exe", "xcopy.exe",
    "expand.exe", "extrac32.exe", "icacls.exe", "cacls.exe", "fsutil.exe", "cscript.exe",
    "wscript.exe", "certutil.exe", "systeminfo.exe", "wmic.exe", "powershell.exe",
    /* Steam's (x86launcher.exe: half a second as a 32-bit game starts, when the app's flexible memory
     * is what the game needs: v00.63 to v00.65 ran out of it then) */
    "steamservice.exe", "steamerrorreporter.exe", "steamerrorreporter64.exe", "x86launcher.exe",
    /* Chrome's: Google's updater, crash handlers */
    "updater.exe", "googleupdate.exe", "googlecrashhandler.exe", "googlecrashhandler64.exe",
    "crashpad_handler.exe", "elevation_service.exe", "notification_helper.exe",
    /* games' crash reporters */
    "unitycrashhandler32.exe", "unitycrashhandler64.exe", "crashreportclient.exe", "bssndrpt.exe",
    "bssndrpt64.exe", "crashsender1403.exe",
    NULL
};

static int outside_allowed( void )
{
    const char *env = getenv( "WINEPS4_OUTSIDE" );
    return outside_state >= 0 && (!env || strcmp( env, "0" ));
}

static int background_program( const char *image )
{
    const char *name = image, *p;
    int i;

    for (p = image; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    for (i = 0; background_programs[i]; i++)
        if (!strcasecmp( name, background_programs[i] )) return 1;
    return 0;
}

/* a pid started outside the 7 (the reaper leaves its end to its socket) */
int ps4_outside_child( int pid )
{
    unsigned int i;

    for (i = 0; i < sizeof(outside_pids) / sizeof(outside_pids[0]); i++)
        if (outside_pids[i] == pid) return 1;
    return 0;
}

/* how many run outside the 7 now */
static int outside_count( void )
{
    int i, n = 0;

    for (i = 0; i < local_count; i++) n += locals[i].local_id < 0;
    return n;
}

/* The PS4 starts a local process of the app, and it says HELLO: 1, with *sock (the desktop's end of
 * its socket), *id and *pid; 0: it did not answer; -1: the PS4 runs 7 (0x80aa0008); -2: it starts
 * none for the app (the first one refused). */
static int start_local( int *sock, int *id_out, int *pid_out )
{
    static const char *const local_argv[] = { LOCAL_ARG, NULL };
    uint32_t param[0x8c / 4];
    struct local_msg msg;
    int sv[2], id;

    if (socketpair( AF_UNIX, SOCK_STREAM, 0, sv ) == -1) return 0;
    fcntl( sv[0], F_SETFD, FD_CLOEXEC );
    fcntl( sv[1], F_SETFD, FD_CLOEXEC );

    /* the request as the PS4's own PS2 emulator makes it: size, the child's socket, crash reports */
    memset( param, 0, sizeof(param) );
    param[0] = sizeof(param);
    param[1] = sv[1];
    param[2] = 1;
    id = sceSystemServiceAddLocalProcess( app_id, "/app0/wineps4.bin", local_argv, param );
    if (id < 0)
    {
        fprintf( stderr, "wineps4: the PS4 did not start a local process (0x%08x)%s\n", id,
                 local_state ? "" : ": programs are forked (close the app with Power > Exit only)" );
        close( sv[0] );
        close( sv[1] );
        if (local_state) return -1;  /* (0x80aa0008: 7 already run, the most an app has) */
        local_state = -1;
        return -2;
    }
    /* HELLO: it runs (our copy of its socket is kept until then: a unix socket whose every
     * descriptor is in flight can arrive emptied) */
    if (!read_msg( sv[0], &msg, LOCAL_TIMEOUT_MS ) || msg.magic != MSG_HELLO)
    {
        fprintf( stderr, "wineps4: local process %d did not answer\n", id );
        sceSystemServiceKillLocalProcess( app_id, id );
        close( sv[0] );
        close( sv[1] );
        if (local_state) return 0;
        local_state = -1;  /* (the first one: the PS4 starts them, but not this way) */
        return -2;
    }
    close( sv[1] );
    *sock = sv[0];
    *id_out = id;
    *pid_out = msg.value;
    return 1;
}

/* The desktop starts a process outside the PS4's 7 (/app0/wineps4.bin OUTSIDE_ARG, its socket as
 * descriptor 3, out of the app's budget), and it says HELLO: 1, with *sock and *pid. The first that
 * fails is tried again in the app's budget; when that fails too, none start outside any more. */
static int start_outside( int *sock, int *pid_out )
{
    static char *outside_argv[] = { (char *)"/app0/wineps4.bin", (char *)OUTSIDE_ARG, NULL };
    static char *no_env[] = { NULL };
    struct local_msg msg;
    int sv[2], pid;

    if (outside_allowed() && outside_pool < OUTSIDE_MIN_POOL)
    {
        char text[512];
        int i, len;

        /* (written once the lock is let go: a full log pipe must not hold it, Task Manager and the
         * Performance log take it from the desktop's loop) */
        len = snprintf( text, sizeof(text), "wineps4: the memory the processes outside the PS4's 7 share has %llu MiB free: "
                        "this one starts among the 7 (Wine's memory outside:", (unsigned long long)(outside_pool >> 20) );
        pthread_mutex_lock( &locals_lock );
        for (i = 0; i < local_count && len < (int)sizeof(text) - 32; i++)
            if (locals[i].local_id < 0)
                len += snprintf( text + len, sizeof(text) - len, " %d %llu MiB", locals[i].pid,
                                 (unsigned long long)(locals[i].stats.memory >> 20) );
        pthread_mutex_unlock( &locals_lock );
        fprintf( stderr, "%s)\n", text );
        return 0;
    }
    while (outside_allowed())
    {
        if (socketpair( AF_UNIX, SOCK_STREAM, 0, sv ) == -1) return 0;
        fcntl( sv[0], F_SETFD, FD_CLOEXEC );
        fcntl( sv[1], F_SETFD, FD_CLOEXEC );
        pid = ps4_spawn_outside( outside_argv[0], outside_argv, no_env, sv[1], outside_budget );
        close( sv[1] );  /* (the child has it as its 3: its end is the socket's end) */
        if (pid > 0)
        {
            outside_pids[outside_next++ % (sizeof(outside_pids) / sizeof(outside_pids[0]))] = pid;
            if (read_msg( sv[0], &msg, LOCAL_TIMEOUT_MS ) && msg.magic == MSG_HELLO)
            {
                if (outside_state != 1)
                    fprintf( stderr, "wineps4: processes past the PS4's 7 start outside it (%s)\n",
                             outside_budget ? "each out of the app's budget" : "in the app's budget" );
                outside_state = 1;
                *sock = sv[0];
                *pid_out = pid;
                return 1;
            }
            kill( pid, SIGKILL );
        }
        fprintf( stderr, "wineps4: a process outside the PS4's 7 did not start (%s%s)\n",
                 pid > 0 ? "no answer" : strerror( errno ), outside_budget ? ", out of the app's budget" : "" );
        close( sv[0] );
        if (outside_state == 1) return 0;  /* (they did before: this one only) */
        if (outside_budget) outside_budget = 0;  /* again, in the app's budget */
        else outside_state = -1;
    }
    return 0;
}

/* Has the PS4 start argv (argv[0] /app0/wineps4.bin) as a local process with the environment envp,
 * fd0, fd1 and fd2 as its standard input, output and error and server_fd as its wineserver socket
 * (-1: none), or starts it outside the PS4's 7 (Windows' background programs first; any once the 7
 * run): its pid, or -errno; -ENOSYS: the PS4 refuses local processes to the app (the caller forks
 * instead). With ps4_spawn_lock held. */
int ps4_local_spawn( char *const argv[], char *const envp[], int fd0, int fd1, int fd2, int server_fd )
{
    union { struct cmsghdr align; char buf[CMSG_SPACE( 4 * sizeof(int) )]; } control;
    struct spawn_header header = { LOCAL_MAGIC };
    const int send_fds[4] = { fd0, fd1, fd2, server_fd };
    const char *image = argv[1] ? argv[1] : "";
    struct local_msg msg;
    struct msghdr m;
    struct cmsghdr *cmsg;
    struct iovec iov;
    int sock = -1, fds[4], nfds = 0, i, id = -1, pid = -1, ret = -EIO, started, outside_now = 0;
    char *strings = NULL, *p;
    size_t size = 0;

    if (local_state < 0) return -ENOSYS;
    if (app_id < 0 && (app_id = sceSystemServiceGetAppIdOfBigApp()) < 0)
    {
        fprintf( stderr, "wineps4: no app id (0x%08x): programs are forked\n", app_id );
        local_state = -1;
        return -ENOSYS;
    }
    for (i = 0; argv[i]; i++) size += strlen( argv[i] ) + 1;
    header.argc = i;
    for (i = 0; envp[i]; i++)
        if (!skip_env( envp[i] )) size += strlen( envp[i] ) + 1, header.envc++;
    header.size = size;
    for (i = 0; i < 4; i++)
        if (send_fds[i] >= 0) fds[nfds++] = send_fds[i], header.fds |= 1 << i;
    if (!(strings = malloc( size ))) return -ENOMEM;
    for (p = strings, i = 0; argv[i]; i++) p = stpcpy( p, argv[i] ) + 1;
    for (i = 0; envp[i]; i++)
        if (!skip_env( envp[i] )) p = stpcpy( p, envp[i] ) + 1;

    /* a background program outside the 7 if it can be; else a local process, outside once 7 run */
    /* where it runs: Windows' background programs and helpers outside the PS4's 7 first, and so does any
     * program while the app's direct memory is short (a local process takes its C library's heap and
     * Windows' memory from it, one outside takes the system's pool instead: v00.58's helpers, started
     * with little of it left, took the rest and Steam's web helper died) */
    started = (background_program( image ) || ps4_vm_direct_free() < LOW_DIRECT_MEMORY) && start_outside( &sock, &pid );
    if (!started)
    {
        const int local = start_local( &sock, &id, &pid );

        if (local == -1 && start_outside( &sock, &pid )) started = 1;
        else if (local == 1) started = 1;
        else
        {
            if (local == -1) ps4_shell_notice( "The PS4 runs 7 Windows processes at once at most: close a program first" );
            ret = local == -2 ? -ENOSYS : local == -1 ? -EAGAIN : -EIO;
            free( strings );
            return ret;
        }
    }

    memset( &m, 0, sizeof(m) );
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (nfds)
    {
        memset( &control, 0, sizeof(control) );
        m.msg_control = control.buf;
        m.msg_controllen = CMSG_SPACE( nfds * sizeof(int) );
        cmsg = CMSG_FIRSTHDR( &m );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN( nfds * sizeof(int) );
        memcpy( CMSG_DATA( cmsg ), fds, nfds * sizeof(int) );
    }
    while (sendmsg( sock, &m, 0 ) == -1)
        if (errno != EINTR) goto failed;
    if (!write_all( sock, strings, size ) || !read_msg( sock, &msg, LOCAL_TIMEOUT_MS ) || msg.magic != MSG_READY)
    {
        fprintf( stderr, "wineps4: %s process %d (pid %d) did not take its hand-off\n", id >= 0 ? "local" : "outside",
                 id, pid );
        goto failed;
    }

    if (id >= 0) local_state = 1;
    pthread_once( &monitor_once, start_monitor );
    pthread_mutex_lock( &locals_lock );
    if (local_count < MAX_PROCS)
    {
        memset( &locals[local_count], 0, sizeof(locals[local_count]) );
        locals[local_count].local_id = id;
        locals[local_count].pid = pid;
        locals[local_count].control = sock;
        locals[local_count].code = -1;
        snprintf( locals[local_count].image, sizeof(locals[local_count].image), "%s", image );
        local_count++;
        sock = -1;
        outside_now = id < 0 ? outside_count() : 0;
    }
    pthread_mutex_unlock( &locals_lock );
    if (sock != -1) goto failed;  /* (more than this table holds) */
    if (id < 0)  /* (after the lock: see start_outside) */
        fprintf( stderr, "wineps4: %s runs outside the PS4's 7 (process %d, %d outside now)\n", image, pid, outside_now );
    ps4_child_started( pid, image );
    if (wake[1] != -1) write( wake[1], "", 1 );
    ret = pid;
    goto done;

failed:
    if (id >= 0) sceSystemServiceKillLocalProcess( app_id, id );
    else kill( pid, SIGKILL );
    if (id >= 0 && !local_state)  /* (the first one: the PS4 starts them, but not this way) */
    {
        local_state = -1;
        ret = -ENOSYS;
    }
done:
    if (sock != -1) close( sock );
    free( strings );
    return ret;
}

/* the system's end of every local process (Power > Exit and Restart): their sockets' ends then
 * reach the monitor; how many there were */
int ps4_local_kill_all( void )
{
    int ids[MAX_PROCS], pids[MAX_PROCS], n, i;

    pthread_mutex_lock( &locals_lock );
    for (n = 0; n < local_count; n++) ids[n] = locals[n].local_id, pids[n] = locals[n].pid;
    pthread_mutex_unlock( &locals_lock );
    for (i = 0; i < n; i++)
    {
        if (ids[i] >= 0) sceSystemServiceKillLocalProcess( app_id, ids[i] );
        kill( pids[i], SIGKILL );
    }
    return n;
}

/* Task Manager's list (shell.h): the local processes and the ones outside the 7, with their
 * programs and last figures */
int ps4_process_list( struct ps4_process_info *list, int max )
{
    int i, n = 0;

    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count && n < max; i++, n++)
    {
        list[n].pid = locals[i].pid;
        snprintf( list[n].image, sizeof(list[n].image), "%s", locals[i].image );
        list[n].cpu_seconds = locals[i].stats.cpu_us / 1e6;
        list[n].memory = locals[i].stats.memory;
        list[n].flexible = locals[i].stats.flexible;
        list[n].shared = locals[i].stats.shared;
        list[n].outside = locals[i].local_id < 0;
    }
    pthread_mutex_unlock( &locals_lock );
    return n;
}

/* the Performance log's figure (shell.h), read without the lock */
unsigned long long ps4_outside_pool( void )
{
    return __atomic_load_n( &outside_pool, __ATOMIC_RELAXED );
}

/* one of them (a stuck wineboot; Task Manager's End task) */
void ps4_local_kill( int pid )
{
    int i, id = -1;

    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count; i++)
        if (locals[i].pid == pid) id = locals[i].local_id;
    pthread_mutex_unlock( &locals_lock );
    if (id >= 0) sceSystemServiceKillLocalProcess( app_id, id );
    kill( pid, SIGKILL );
}

/* Is pid one of the app's local processes (among the PS4's 7, not outside)? wineserver's sections in
 * direct memory are theirs only (port/ps4_wine_vm.c) */
int ps4_pid_local( int pid )
{
    int i, local = 0;

    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count; i++)
        if (locals[i].pid == pid) local = locals[i].local_id >= 0;
    pthread_mutex_unlock( &locals_lock );
    return local;
}

/* The memory guard (Settings > Diagnostics > Memory guard; WINEPS4_MEMORY_GUARD=0: off). The local
 * processes all take the app's flexible memory, about 1 GiB between them, and the PS4 doesn't refuse
 * a page when it is gone: the thread that touched it waits in the kernel for good, and the app's
 * processes stood one after another (v00.63 to v00.65, each time as a game started from Steam, whose
 * web helper held 465 MiB of it). Four times a second the desktop reads what is free: under
 * GUARD_WARN it tells who holds it, under GUARD_LOW it ends the web helper (a launcher's browser,
 * which the launcher starts again) that holds the most, once in GUARD_PAUSE seconds. Without printf
 * or the desktop's locks: it runs when anything else may stand. */
#define GUARD_WARN  (192ull << 20)
#define GUARD_LOW   (96ull << 20)
#define GUARD_PAUSE 30
extern void (*ps4_kstate_log)( const char *line );

static const char *const web_helpers[] =
{
    "steamwebhelper.exe", "epicwebhelper.exe", "qtwebengineprocess.exe", "msedgewebview2.exe",
    "cefsharp.browsersubprocess.exe", NULL
};

static const char *image_name( const char *image )
{
    const char *name = image, *p;

    for (p = image; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

static int is_web_helper( const char *image )
{
    int i;

    for (i = 0; web_helpers[i]; i++)
        if (!strcasecmp( image_name( image ), web_helpers[i] )) return 1;
    return 0;
}

static void guard_log( struct ps4_line *l )
{
    l->text[l->len] = 0;
    if (ps4_kstate_log) ps4_kstate_log( l->text );
}

/* who holds the app's flexible memory: its four largest local processes */
static void guard_holders( struct ps4_line *l )
{
    struct { int pid; uint64_t flexible, shared; char name[40]; } top[4] = { { 0 } };
    int i, j, n = 0;

    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count; i++)
    {
        const uint64_t size = locals[i].stats.flexible + locals[i].stats.shared;

        if (locals[i].local_id < 0) continue;  /* (outside the 7: the system's memory) */
        for (j = n < 4 ? n : 4; j > 0 && top[j - 1].flexible + top[j - 1].shared < size; j--)
            if (j < 4) top[j] = top[j - 1];
        if (j >= 4) continue;
        top[j].pid = locals[i].pid;
        top[j].flexible = locals[i].stats.flexible;
        top[j].shared = locals[i].stats.shared;
        snprintf( top[j].name, sizeof(top[j].name), "%s", image_name( locals[i].image ) );
        if (n < 4) n++;
    }
    pthread_mutex_unlock( &locals_lock );
    for (i = 0; i < n; i++)
    {
        ps4_line_str( l, i ? ", " : " (" );
        ps4_line_str( l, top[i].name );
        ps4_line_str( l, " " );
        ps4_line_num( l, (long long)(top[i].flexible >> 20) );
        ps4_line_str( l, " MiB flexible and " );
        ps4_line_num( l, (long long)(top[i].shared >> 20) );
        ps4_line_str( l, " MiB shared" );
    }
    if (n) ps4_line_str( l, ")" );
}

static void *memory_guard( void *arg )
{
    const struct timespec tick = { 0, 250000000 };
    time_t last_end = 0;
    int warned = 0;
    sigset_t set;

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    ps4_thread_name( "memory guard" );
    ps4_touch_stack( 32 << 10 );  /* (pages first touched when memory is gone: a fault that stands) */
    for (;;)
    {
        size_t free_size = 0;
        uint64_t most = 0;
        int i, victim = -1;
        char name[40] = "";
        struct ps4_line l;
        struct timespec now;

        nanosleep( &tick, NULL );
        if (sceKernelAvailableFlexibleMemorySize( &free_size )) continue;
        if (free_size >= GUARD_WARN)
        {
            warned = 0;
            continue;
        }
        if (!warned)
        {
            warned = 1;
            l.len = 0;
            ps4_line_str( &l, "wineps4: memory guard: the app's flexible memory is down to " );
            ps4_line_num( &l, (long long)(free_size >> 20) );
            ps4_line_str( &l, " MiB free" );
            guard_holders( &l );
            guard_log( &l );
        }
        if (free_size >= GUARD_LOW) continue;
        clock_gettime( CLOCK_MONOTONIC, &now );
        if (last_end && now.tv_sec - last_end < GUARD_PAUSE) continue;
        pthread_mutex_lock( &locals_lock );
        for (i = 0; i < local_count; i++)
        {
            const uint64_t size = locals[i].stats.flexible + locals[i].stats.shared;

            if (locals[i].local_id < 0 || !is_web_helper( locals[i].image ) || size < most) continue;
            most = size;
            victim = locals[i].pid;
            snprintf( name, sizeof(name), "%s", image_name( locals[i].image ) );
        }
        pthread_mutex_unlock( &locals_lock );
        l.len = 0;
        ps4_line_str( &l, "wineps4: memory guard: " );
        ps4_line_num( &l, (long long)(free_size >> 20) );
        ps4_line_str( &l, " MiB of the app's flexible memory free" );
        if (victim < 0)
        {
            ps4_line_str( &l, ", and no web helper to end: the app may stand" );
            guard_holders( &l );
            guard_log( &l );
            last_end = now.tv_sec;  /* (told once a pause) */
            continue;
        }
        last_end = now.tv_sec;
        ps4_local_kill( victim );  /* (first: the notice takes the desktop's lock) */
        ps4_line_str( &l, ": " );
        ps4_line_str( &l, name );
        ps4_line_str( &l, " (process " );
        ps4_line_num( &l, victim );
        ps4_line_str( &l, ", " );
        ps4_line_num( &l, (long long)(most >> 20) );
        ps4_line_str( &l, " MiB of it) ended, its launcher starts it again" );
        guard_log( &l );
        ps4_shell_notice( "Memory for programs was nearly used up: the launcher's window was restarted to free it" );
    }
    return NULL;
}

void ps4_memory_guard_start( void )
{
    const char *setting = getenv( "WINEPS4_MEMORY_GUARD" );
    pthread_t thread;

    if (setting && !strcmp( setting, "0" )) return;
    if (!pthread_create( &thread, NULL, memory_guard, NULL )) pthread_detach( thread );
}

/* the end of the desktop: the local process ends at once */
static void *desktop_watch( void *arg )
{
    char c;

    while (read( (int)(long)arg, &c, 1 ) == -1 && errno == EINTR) ;
    ps4_process_exit_now( 1 );
}

extern unsigned long long ps4_vm_backed_bytes( void );
extern void ps4_heartbeat_check( void );
extern int ps4_memory_census( uint64_t *flexible, uint64_t *shared );
extern void ps4_memory_breakdown( const char *who );

/* a local process's program (its file name), for its own lines */
static char program_name[64] = "a program";

/* the process's figures for the desktop's Task Manager, every second (one write each: they don't
 * mix with the exit code's), its mappings every 5 s, what they are once a minute while they are many
 * (BREAKDOWN_FLEXIBLE); and whether the desktop still beats (port/ps4_kstate.c: before the write,
 * which a desktop that stands stops reading) */
#define BREAKDOWN_FLEXIBLE (192ull << 20)
static void *stats_thread( void *arg )
{
    const int sock = (int)(long)arg;
    uint64_t flexible = 0, shared = 0;
    unsigned int tick = 0, last_breakdown = 0;
    sigset_t all;

    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, NULL );  /* (Wine's signals are its own threads') */
    ps4_thread_name( "stats" );
    for (;;)
    {
        struct { struct local_msg msg; struct local_stats stats; } m = { { MSG_STATS, 0 } };
        struct rusage usage;

        ps4_heartbeat_check();
        if (!(tick++ % 5)) ps4_memory_census( &flexible, &shared );
        if (flexible >= BREAKDOWN_FLEXIBLE && (!last_breakdown || tick - last_breakdown >= 60))
        {
            last_breakdown = tick;
            ps4_memory_breakdown( program_name );
        }
        m.stats.flexible = flexible;
        m.stats.shared = shared;
        if (!getrusage( RUSAGE_SELF, &usage ))
            m.stats.cpu_us = (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 +
                             usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
        m.stats.memory = ps4_vm_backed_bytes();
        if (ps4_outside_process)
        {
            size_t pool = 0;
            if (!sceKernelAvailableFlexibleMemorySize( &pool )) m.stats.pool = pool ? pool : 1;
        }
        if (!write_all( sock, &m, sizeof(m) )) break;
        sleep( 1 );
    }
    return NULL;
}

/* Is this a local process (argv has LOCAL_ARG: 1) or one the desktop started outside the PS4's 7
 * (OUTSIDE_ARG: 2)? */
int ps4_local_child( int argc, char **argv )
{
    int i;

    for (i = 0; i < argc; i++)
    {
        if (argv[i] && !strcmp( argv[i], LOCAL_ARG )) return 1;
        if (argv[i] && !strcmp( argv[i], OUTSIDE_ARG )) return 2;
    }
    return 0;
}

/* A local process: the desktop's hand-off over the system's socket (outside the 7: its descriptor
 * 3). Sets the environment and the standard descriptors and returns the program's arguments; 0: no
 * hand-off. */
int ps4_local_child_setup( int outside, int *argc_out, char ***argv_out )
{
    union { struct cmsghdr align; char buf[CMSG_SPACE( 4 * sizeof(int) )]; } control;
    struct spawn_header header;
    struct local_msg msg = { MSG_HELLO, (int32_t)getpid() };
    struct msghdr m;
    struct cmsghdr *cmsg;
    struct iovec iov;
    pthread_attr_t attr;
    pthread_t thread;
    int sock = outside ? 3 : sceSystemServiceGetParentSocket(), got[4], ngot = 0, fds[4] = { -1, -1, -1, -1 }, i, k;
    char **list, *strings, *p, *end, number[16];
    ssize_t n;

    if (sock < 0 || fcntl( sock, F_GETFD ) == -1) sock = 3;  /* (where the system puts it) */
    if (fcntl( sock, F_GETFD ) == -1 || !write_all( sock, &msg, sizeof(msg) )) return 0;

    memset( &m, 0, sizeof(m) );
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control.buf;
    m.msg_controllen = sizeof(control.buf);
    while ((n = recvmsg( sock, &m, MSG_WAITALL )) == -1 && errno == EINTR) ;
    for (cmsg = CMSG_FIRSTHDR( &m ); n > 0 && cmsg; cmsg = CMSG_NXTHDR( &m, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            for (k = 0; k < (int)((cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int)) && ngot < 4; k++)
                memcpy( &got[ngot++], CMSG_DATA( cmsg ) + k * sizeof(int), sizeof(int) );
    if (n != sizeof(header) || header.magic != LOCAL_MAGIC || !header.argc || header.size > SPAWN_MAX ||
        header.argc + header.envc > SPAWN_MAX / 2)
        return 0;
    for (i = k = 0; i < 4; i++)
        if ((header.fds & (1 << i)) && k < ngot) fds[i] = got[k++];

    /* out of the way first (a fresh descriptor table: what came may sit at 0, 1 or 2), then in place */
    for (i = 0; i < 4; i++)
        if (fds[i] >= 0)
        {
            const int high = fcntl( fds[i], F_DUPFD, 20 );
            close( fds[i] );
            fds[i] = high;
        }
    for (i = 0; i < 3; i++)
    {
        if (fds[i] >= 0)
        {
            dup2( fds[i], i );
            close( fds[i] );
        }
        else if (fcntl( i, F_GETFD ) == -1)
        {
            const int null = open( "/dev/null", O_RDWR );
            if (null >= 0 && null != i)
            {
                dup2( null, i );
                close( null );
            }
        }
    }

    if (!(strings = malloc( header.size + 1 )) || !(list = calloc( header.argc + 1, sizeof(*list) ))) return 0;
    if (!read_all( sock, strings, header.size )) return 0;
    strings[header.size] = 0;
    for (p = strings, end = strings + header.size, i = 0; i < (int)(header.argc + header.envc); i++)
    {
        if (p >= end) return 0;
        if (i < (int)header.argc) list[i] = p;
        else putenv( p );  /* (kept: strings stays) */
        p += strlen( p ) + 1;
    }
    unsetenv( "WINEPS4_LIFELINE" );
    if (fds[3] >= 0)
    {
        snprintf( number, sizeof(number), "%d", fds[3] );
        setenv( "WINESERVERSOCKET", number, 1 );
    }

    msg.magic = MSG_READY;
    msg.value = 0;
    if (!write_all( sock, &msg, sizeof(msg) )) return 0;
    fcntl( sock, F_SETFD, FD_CLOEXEC );
    ps4_control_fd = sock;
    if (!outside) ps4_section_probe();  /* (may wineserver's sections be in direct memory?) */
    snprintf( program_name, sizeof(program_name), "%s", image_name( header.argc > 1 ? list[1] : list[0] ) );
    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 64 << 10 );
    if (!pthread_create( &thread, &attr, desktop_watch, (void *)(long)sock )) pthread_detach( thread );
    if (!pthread_create( &thread, &attr, stats_thread, (void *)(long)sock )) pthread_detach( thread );
    pthread_attr_destroy( &attr );
    *argc_out = header.argc;
    *argv_out = list;
    return 1;
}

/* --- A program's side -------------------------------------------------------------------------- */

/* Has the desktop start path (argv[0] is path), with fd0/fd1 as its standard input/output and
 * server_fd kept at its number (the environment's WINESERVERSOCKET): its pid, or -1. When the desktop
 * can't be reached, the program starts it itself (ps4_spawn). */
pid_t ps4_spawn_program( const char *path, char **argv, char **envp, int fd0, int fd1, int server_fd )
{
    static int no_desktop;
    struct spawn_header header = { SPAWN_MAGIC };
    struct sockaddr_un addr;
    union { struct cmsghdr align; char buf[CMSG_SPACE( 3 * sizeof(int) )]; } control;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec iov;
    int fds[3], nfds = 0, sock, i, answer;
    char *strings, *p;
    size_t size = strlen( path ) + 1;

    for (i = 0; argv[i]; i++) size += strlen( argv[i] ) + 1;
    header.argc = i + 1;
    for (i = 0; envp[i]; i++) size += strlen( envp[i] ) + 1;
    header.envc = i;
    header.size = size;
    if (fd0 >= 0) { fds[nfds++] = fd0; header.fds |= 1; }
    if (fd1 >= 0) { fds[nfds++] = fd1; header.fds |= 2; }
    if (server_fd >= 0) { fds[nfds++] = server_fd; header.fds |= 4; }

    if (no_desktop || size > SPAWN_MAX || (sock = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1)
        return ps4_spawn( path, argv, envp, fd0, fd1 );
    fcntl( sock, F_SETFD, FD_CLOEXEC );
    socket_address( &addr );
    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
    {
        fprintf( stderr, "wineps4: process %d: the desktop does not start processes (%s): started here\n",
                 (int)getpid(), strerror( errno ) );
        close( sock );
        no_desktop = 1;
        return ps4_spawn( path, argv, envp, fd0, fd1 );
    }

    if (!(strings = malloc( size )))
    {
        close( sock );
        errno = ENOMEM;
        return -1;
    }
    p = strings;
    p = stpcpy( p, path ) + 1;
    for (i = 0; argv[i]; i++) p = stpcpy( p, argv[i] ) + 1;
    for (i = 0; envp[i]; i++) p = stpcpy( p, envp[i] ) + 1;

    memset( &msg, 0, sizeof(msg) );
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (nfds)
    {
        memset( &control, 0, sizeof(control) );
        msg.msg_control = control.buf;
        msg.msg_controllen = CMSG_SPACE( nfds * sizeof(int) );
        cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN( nfds * sizeof(int) );
        memcpy( CMSG_DATA( cmsg ), fds, nfds * sizeof(int) );
    }
    answer = -EPIPE;
    while (sendmsg( sock, &msg, 0 ) == -1)
        if (errno != EINTR) goto done;
    if (!write_all( sock, strings, size ) || !read_all( sock, &answer, sizeof(answer) )) answer = -EPIPE;
done:
    free( strings );
    close( sock );
    if (answer < 0)
    {
        errno = -answer;
        return -1;
    }
    return answer;
}

/* --- The desktop's side ------------------------------------------------------------------------ */

/* one request on conn: the process started, or -errno */
static int serve( int conn )
{
    struct spawn_header header;
    union { struct cmsghdr align; char buf[CMSG_SPACE( 3 * sizeof(int) )]; } control;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec iov;
    int fds[3] = { -1, -1, -1 }, got[3], ngot = 0, i, k, fd0 = -1, fd1 = -1, server = -1, ret;
    char **list = NULL, *strings = NULL, *p, *end, server_env[40];
    ssize_t n;

    memset( &msg, 0, sizeof(msg) );
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);
    while ((n = recvmsg( conn, &msg, MSG_WAITALL )) == -1 && errno == EINTR) ;
    for (cmsg = CMSG_FIRSTHDR( &msg ); n > 0 && cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            for (k = 0; k < (int)((cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int)) && ngot < 3; k++)
                memcpy( &got[ngot++], CMSG_DATA( cmsg ) + k * sizeof(int), sizeof(int) );
    ret = -EPROTO;
    if (n != sizeof(header) || header.magic != SPAWN_MAGIC || header.argc < 2 || header.size > SPAWN_MAX ||
        header.argc + header.envc > SPAWN_MAX / 2)
        goto done;
    for (i = k = 0; i < 3; i++)
        if ((header.fds & (1 << i)) && k < ngot) fds[i] = got[k++];
    fd0 = fds[0];
    fd1 = fds[1];
    server = fds[2];
    if (fd0 >= 0) fcntl( fd0, F_SETFD, FD_CLOEXEC );  /* (the child gets them as 0 and 1) */
    if (fd1 >= 0) fcntl( fd1, F_SETFD, FD_CLOEXEC );

    ret = -ENOMEM;
    if (!(strings = malloc( header.size + 1 )) || !(list = calloc( header.argc + header.envc + 2, sizeof(*list) )))
        goto done;
    ret = -EPROTO;
    if (!read_all( conn, strings, header.size )) goto done;
    strings[header.size] = 0;
    /* path, args, NULL, environment, NULL */
    for (p = strings, end = strings + header.size, i = 0; i < (int)(header.argc + header.envc); i++)
    {
        if (p >= end) goto done;
        list[i < (int)header.argc ? i : i + 1] = p;
        p += strlen( p ) + 1;
    }
    /* a local process (its standard error is the desktop's: wine.log), else forked */
    if ((ret = ps4_local_spawn( list + 1, list + header.argc + 1, fd0, fd1, 2, server )) != -ENOSYS) goto done;
    if (server >= 0)  /* the socket is at another number here */
        for (i = header.argc + 1; list[i]; i++)
            if (!strncmp( list[i], "WINESERVERSOCKET=", 17 ))
            {
                snprintf( server_env, sizeof(server_env), "WINESERVERSOCKET=%d", server );
                list[i] = server_env;
            }
    ret = ps4_spawn( list[0], list + 1, list + header.argc + 1, fd0, fd1 );
    if (ret == -1) ret = -errno;
    else ps4_child_started( ret, header.argc > 2 ? list[2] : list[0] );

done:
    for (i = 0; i < ngot; i++) close( got[i] );
    free( list );
    free( strings );
    return ret;
}

static void *broker_thread( void *arg )
{
    const int listener = (int)(long)arg;

    ps4_thread_register();  /* (the freeze report's) */
    ps4_thread_name( "spawn broker" );
    for (;;)
    {
        int conn = accept( listener, NULL, NULL ), answer;

        if (conn == -1)
        {
            if (errno != EINTR && errno != ECONNABORTED) usleep( 100000 );
            continue;
        }
        fcntl( conn, F_SETFD, FD_CLOEXEC );
        pthread_mutex_lock( &ps4_spawn_lock );
        answer = serve( conn );
        pthread_mutex_unlock( &ps4_spawn_lock );
        if (answer < 0) fprintf( stderr, "wineps4: desktop: a process did not start: %s\n", strerror( -answer ) );
        write_all( conn, &answer, sizeof(answer) );
        close( conn );
    }
    return NULL;
}

/* the desktop: starts taking the programs' requests */
int ps4_spawn_broker_start( void )
{
    struct sockaddr_un addr;
    pthread_t thread;
    int listener = socket( AF_UNIX, SOCK_STREAM, 0 );

    if (listener == -1) return 0;
    fcntl( listener, F_SETFD, FD_CLOEXEC );
    socket_address( &addr );
    unlink( SPAWN_SOCKET );
    if (bind( listener, (struct sockaddr *)&addr, sizeof(addr) ) == -1 || listen( listener, 32 ) == -1 ||
        pthread_create( &thread, NULL, broker_thread, (void *)(long)listener ))
    {
        fprintf( stderr, "wineps4: desktop: %s: %s\n", SPAWN_SOCKET, strerror( errno ) );
        close( listener );
        return 0;
    }
    pthread_detach( thread );
    return 1;
}
