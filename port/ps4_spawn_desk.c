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
#include <unistd.h>

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
extern void ps4_child_started( int pid );

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
 * forks the programs as before (and wine.log says so). */

#define LOCAL_ARG        "--wineps4-local"  /* the argument the PS4 starts a local process with */
#define LOCAL_MAGIC      0x504c3457         /* "W4LP": the desktop's hand-off */
#define MSG_HELLO        0x4f4c4548         /* "HELO", the pid */
#define MSG_READY        0x59444552         /* "REDY" */
#define MSG_EXIT         0x54495845         /* "EXIT", the exit code (port/ps4_wine_port.c) */
#define MAX_LOCAL        16                 /* SCE_LNC_UTIL_MAX_LOCAL_PROCESS_NUM */
#define LOCAL_TIMEOUT_MS 15000

struct local_msg
{
    uint32_t magic;
    int32_t value;
};

int sceSystemServiceAddLocalProcess( int app_id, const char *path, const char *const *argv, const void *param );
int sceSystemServiceKillLocalProcess( int app_id, int local_id );
int sceSystemServiceGetAppIdOfBigApp( void );
int sceSystemServiceGetParentSocket( void );

extern void ps4_process_ended( int pid, int code );
extern void ps4_process_exit_now( int status ) __attribute__((noreturn));
extern int ps4_control_fd;

static pthread_mutex_t locals_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int local_id, pid, control, code; } locals[MAX_LOCAL];
static int local_count;
static int local_state;            /* 0: not tried yet, 1: the PS4 starts them, -1: refused */
static int app_id = -1;
static int wake[2] = { -1, -1 };   /* wakes the monitor when a process is added */
static pthread_once_t monitor_once = PTHREAD_ONCE_INIT;

/* a message from fd within timeout_ms: 1, or 0 (nothing in time, or the socket's end) */
static int read_msg( int fd, struct local_msg *msg, int timeout_ms )
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ret;

    while ((ret = poll( &pfd, 1, timeout_ms )) == -1 && errno == EINTR) ;
    return ret == 1 && read_all( fd, msg, sizeof(*msg) );
}

/* a message on a local process's socket: its exit code, or its end */
static void control_event( int fd )
{
    struct local_msg msg;
    ssize_t got;
    int i, pid, code, local_id;

    while ((got = read( fd, &msg, sizeof(msg) )) == -1 && errno == EINTR) ;
    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count && locals[i].control != fd; i++) ;
    if (i == local_count || got > 0)
    {
        if (i < local_count && got == sizeof(msg) && msg.magic == MSG_EXIT) locals[i].code = msg.value;
        pthread_mutex_unlock( &locals_lock );
        return;
    }
    pid = locals[i].pid;
    code = locals[i].code;
    local_id = locals[i].local_id;
    close( fd );
    locals[i] = locals[--local_count];
    pthread_mutex_unlock( &locals_lock );
    sceSystemServiceKillLocalProcess( app_id, local_id );  /* (its entry, if the system still has one) */
    ps4_process_ended( pid, code );
}

/* the desktop: every local process's socket, for its end */
static void *local_monitor( void *arg )
{
    struct pollfd fds[MAX_LOCAL + 1];
    char buf[16];
    int n, i;

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

/* Has the PS4 start argv (argv[0] /app0/wineps4.bin) as a local process with the environment envp,
 * fd0, fd1 and fd2 as its standard input, output and error and server_fd as its wineserver socket
 * (-1: none): its pid, or -errno; -ENOSYS: the PS4 refuses local processes to the app (the caller
 * forks instead). With ps4_spawn_lock held. */
int ps4_local_spawn( char *const argv[], char *const envp[], int fd0, int fd1, int fd2, int server_fd )
{
    static const char *const local_argv[] = { LOCAL_ARG, NULL };
    union { struct cmsghdr align; char buf[CMSG_SPACE( 4 * sizeof(int) )]; } control;
    struct spawn_header header = { LOCAL_MAGIC };
    const int send_fds[4] = { fd0, fd1, fd2, server_fd };
    uint32_t param[0x8c / 4];
    struct local_msg msg;
    struct msghdr m;
    struct cmsghdr *cmsg;
    struct iovec iov;
    int sv[2], fds[4], nfds = 0, i, id, pid, ret = -EIO;
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
    if (socketpair( AF_UNIX, SOCK_STREAM, 0, sv ) == -1)
    {
        ret = -errno;
        free( strings );
        return ret;
    }
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
        ret = local_state ? -EAGAIN : -ENOSYS;
        if (!local_state) local_state = -1;
        goto done;
    }
    /* HELLO: it runs (our copy of its socket is kept until then: a unix socket whose every
     * descriptor is in flight can arrive emptied) */
    if (!read_msg( sv[0], &msg, LOCAL_TIMEOUT_MS ) || msg.magic != MSG_HELLO)
    {
        fprintf( stderr, "wineps4: local process %d did not answer\n", id );
        goto failed;
    }
    pid = msg.value;
    close( sv[1] );
    sv[1] = -1;

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
    while (sendmsg( sv[0], &m, 0 ) == -1)
        if (errno != EINTR) goto failed;
    if (!write_all( sv[0], strings, size ) || !read_msg( sv[0], &msg, LOCAL_TIMEOUT_MS ) || msg.magic != MSG_READY)
    {
        fprintf( stderr, "wineps4: local process %d (pid %d) did not take its hand-off\n", id, pid );
        goto failed;
    }

    local_state = 1;
    pthread_once( &monitor_once, start_monitor );
    pthread_mutex_lock( &locals_lock );
    if (local_count < MAX_LOCAL)
    {
        locals[local_count].local_id = id;
        locals[local_count].pid = pid;
        locals[local_count].control = sv[0];
        locals[local_count].code = -1;
        local_count++;
        sv[0] = -1;
    }
    pthread_mutex_unlock( &locals_lock );
    if (sv[0] != -1) goto failed;  /* (more than the PS4 allows: refused above already) */
    ps4_child_started( pid );
    if (wake[1] != -1) write( wake[1], "", 1 );
    ret = pid;
    goto done;

failed:
    sceSystemServiceKillLocalProcess( app_id, id );
    if (!local_state)  /* (the first one: the PS4 starts them, but not this way) */
    {
        local_state = -1;
        ret = -ENOSYS;
    }
done:
    if (sv[0] != -1) close( sv[0] );
    if (sv[1] != -1) close( sv[1] );
    free( strings );
    return ret;
}

/* the system's end of every local process (Power > Exit and Restart): their sockets' ends then
 * reach the monitor; how many there were */
int ps4_local_kill_all( void )
{
    int ids[MAX_LOCAL], pids[MAX_LOCAL], n, i;

    pthread_mutex_lock( &locals_lock );
    for (n = 0; n < local_count; n++) ids[n] = locals[n].local_id, pids[n] = locals[n].pid;
    pthread_mutex_unlock( &locals_lock );
    for (i = 0; i < n; i++)
    {
        sceSystemServiceKillLocalProcess( app_id, ids[i] );
        kill( pids[i], SIGKILL );
    }
    return n;
}

/* one of them (a stuck wineboot) */
void ps4_local_kill( int pid )
{
    int i, id = -1;

    pthread_mutex_lock( &locals_lock );
    for (i = 0; i < local_count; i++)
        if (locals[i].pid == pid) id = locals[i].local_id;
    pthread_mutex_unlock( &locals_lock );
    if (id != -1) sceSystemServiceKillLocalProcess( app_id, id );
    kill( pid, SIGKILL );
}

/* the end of the desktop: the local process ends at once */
static void *desktop_watch( void *arg )
{
    char c;

    while (read( (int)(long)arg, &c, 1 ) == -1 && errno == EINTR) ;
    ps4_process_exit_now( 1 );
}

/* Is this a local process (argv has LOCAL_ARG)? */
int ps4_local_child( int argc, char **argv )
{
    int i;

    for (i = 0; i < argc; i++)
        if (argv[i] && !strcmp( argv[i], LOCAL_ARG )) return 1;
    return 0;
}

/* A local process: the desktop's hand-off over the system's socket. Sets the environment and the
 * standard descriptors and returns the program's arguments; 0: no hand-off. */
int ps4_local_child_setup( int *argc_out, char ***argv_out )
{
    union { struct cmsghdr align; char buf[CMSG_SPACE( 4 * sizeof(int) )]; } control;
    struct spawn_header header;
    struct local_msg msg = { MSG_HELLO, (int32_t)getpid() };
    struct msghdr m;
    struct cmsghdr *cmsg;
    struct iovec iov;
    pthread_attr_t attr;
    pthread_t thread;
    int sock = sceSystemServiceGetParentSocket(), got[4], ngot = 0, fds[4] = { -1, -1, -1, -1 }, i, k;
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
    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 64 << 10 );
    if (!pthread_create( &thread, &attr, desktop_watch, (void *)(long)sock )) pthread_detach( thread );
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
    else ps4_child_started( ret );

done:
    for (i = 0; i < ngot; i++) close( got[i] );
    free( list );
    free( strings );
    return ret;
}

static void *broker_thread( void *arg )
{
    const int listener = (int)(long)arg;

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
