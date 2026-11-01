/* WinePS4: every program process is a child of the desktop's process.
 *
 * A process Wine starts (CreateProcess: ntdll's spawn_process) is not forked by the program that
 * asks for it: the program sends the command line, the environment and its descriptors (standard
 * input and output, the wineserver socket) to the desktop over /data/WinePS4/.spawn, and the
 * desktop starts it (ps4_spawn) and answers with its pid. Started by the program, a process was
 * left without a parent when that program ended first (explorer.exe after window.exe,
 * winemenubuilder after wineboot): the desktop could neither collect it nor wait for it, and the
 * PS4 froze closing the app while it was there. Now the desktop ends and collects every one of them
 * before it exits (ps4_end_programs).
 *
 * Built with ps4_wine_compat.h: the kernel's (FreeBSD) msghdr, cmsghdr and sockaddr_un. */
#include "ps4_wine_compat.h"

#include <errno.h>
#include <fcntl.h>
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
