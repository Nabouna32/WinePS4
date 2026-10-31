"""WinePS4: a process per Windows program, applied once to the Wine tree after the other edits.

The process probe showed that the app can fork and execve a program of its package. So Wine works
as everywhere else: the desktop process runs wineserver (a thread) listening on its socket, and every
Windows program is a process of its own, /app0/wineps4.bin started by the desktop or by Wine's
CreateProcess (fork, fork, execv), connecting to that socket.

  - wineserver: ps4_wineserver_main( -1 ) listens (open_master_socket, never times out)
  - the socket's path is absolute on both sides: the PS4 kernel turns relative names down, and
    bind/connect don't go through ps4-kit's file layer
  - ntdll connects as on other systems (no server of its own); start_server can't exec one
  - a new process is /app0/wineps4.bin (loader_exec), with WINESERVERSOCKET as usual
  - signals to threads of other processes: thr_kill2 (ps4_thread_kill2, port/ps4_wine_port.c)

  python3 scripts/apply-multiprocess-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


# --- wineserver: listening -----------------------------------------------------------------------
edit('server/main.c', '''    init_limits();
    sock_init();
    ps4_server_dir_init();
    if (debug_level) fprintf( stderr, "wineserver: starting in-process\\n" );''', '''    init_limits();
    sock_init();
    if (client_fd < 0)
    {
        /* the app's server: every program connects to its socket; it stays when none runs */
        master_socket_timeout = TIMEOUT_INFINITE;
        open_master_socket();
    }
    else ps4_server_dir_init();
    if (debug_level) fprintf( stderr, "wineserver: starting in-process\\n" );''')
edit('server/main.c', '''    /* what master_socket_poll_event does for a client that connects */
    fcntl( client_fd, F_SETFL, O_NONBLOCK );
    if (!(process = create_process( client_fd, NULL, 0, NULL, NULL, NULL, 0, NULL )))
        fatal_error( "wineserver: could not create the first process\\n" );
    create_thread( -1, process, NULL );  /* sends ntdll its request pipe and the protocol version */
    release_object( process );
    main_loop();''', '''    if (client_fd >= 0)
    {
        /* what master_socket_poll_event does for a client that connects */
        fcntl( client_fd, F_SETFL, O_NONBLOCK );
        if (!(process = create_process( client_fd, NULL, 0, NULL, NULL, NULL, 0, NULL )))
            fatal_error( "wineserver: could not create the first process\\n" );
        create_thread( -1, process, NULL );  /* sends ntdll its request pipe and the protocol version */
        release_object( process );
    }
    main_loop();''')

edit('server/request.c', '''    strcpy( addr.sun_path, server_socket_name );
    slen = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path) + 1;''', '''#ifdef __PS4_WINE__
    /* absolute: the PS4 kernel takes no relative names */
    snprintf( addr.sun_path, sizeof(addr.sun_path), "%s/%s", server_dir, server_socket_name );
#else
    strcpy( addr.sun_path, server_socket_name );
#endif
    slen = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path) + 1;''')

edit('server/ptrace.c', '''#elif defined(__PS4_WINE__)
    return ps4_thread_kill( pid, sig );''', '''#elif defined(__PS4_WINE__)
    return ps4_thread_kill2( tgid, pid, sig );''')

# --- ntdll: a client like everywhere else ----------------------------------------------------------
edit('dlls/ntdll/unix/server.c', '''#ifdef __PS4_WINE__
extern int ps4_start_wineserver( void );
#endif
static int server_connect(void)
{
#ifdef __PS4_WINE__
    /* the prefix with its dosdevices links, then the server as a thread of this process */
    initial_cwd = setup_config_dir();
    fchdir( initial_cwd );
    return ps4_start_wineserver();
#endif
    struct sockaddr_un addr;''', '''static int server_connect(void)
{
    struct sockaddr_un addr;''')
edit('dlls/ntdll/unix/server.c', '''        addr.sun_family = AF_UNIX;
        strcpy( addr.sun_path, SOCKETNAME );''', '''        addr.sun_family = AF_UNIX;
#ifdef __PS4_WINE__
        /* absolute: the PS4 kernel takes no relative names */
        snprintf( addr.sun_path, sizeof(addr.sun_path), "%s/%s", server_dir, SOCKETNAME );
#else
        strcpy( addr.sun_path, SOCKETNAME );
#endif''')

edit('dlls/ntdll/unix/loader.c', '''void start_server( BOOL debug )
{
    static BOOL started;  /* we only try once */''', '''void start_server( BOOL debug )
{
#ifdef __PS4_WINE__
    /* the desktop process runs the server: a program can't start one */
    static BOOL said;
    if (!said) MESSAGE( "wine: wineserver is not running (WinePS4's desktop starts it)\\n" );
    said = TRUE;
    return;
#endif
    static BOOL started;  /* we only try once */''')

edit('dlls/ntdll/unix/loader.c', '''static NTSTATUS loader_exec( char **argv, WORD machine )
{
    static char noexec[] = "WINELOADERNOEXEC=1";

    putenv( noexec );
''', '''static NTSTATUS loader_exec( char **argv, WORD machine )
{
    static char noexec[] = "WINELOADERNOEXEC=1";

    putenv( noexec );
#ifdef __PS4_WINE__
    /* the app is the loader: a program process of it (port/ps4_main.c, WINESERVERSOCKET) */
    argv[1] = (char *)"/app0/wineps4.bin";
    execv( argv[1], argv + 1 );
    return STATUS_INVALID_IMAGE_FORMAT;
#endif
''')
print('ok')
