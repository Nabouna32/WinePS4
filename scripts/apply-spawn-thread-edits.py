"""WinePS4: a program starts a process from a thread of the app's own (port/ps4_wine_port.c,
ps4_spawn). Applied once to the Wine tree after apply-spawn-trace-edits.py.

v00.29: explorer.exe's child was still there but had written nothing when its socket closed: it died
at the fork. Wine's threads run on stacks it allocated (direct memory on the PS4), which a forked
child doesn't have. spawn_process now builds the loader's arguments and environment (what
exec_wineloader/loader_exec put there) in malloc'd memory, and ps4_spawn forks and execs from a
thread with an ordinary stack.

  python3 scripts/apply-spawn-thread-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


edit('dlls/ntdll/unix/process.c', '''static NTSTATUS spawn_process( const RTL_USER_PROCESS_PARAMETERS *params, int socketfd,''', '''#ifdef __PS4_WINE__
extern pid_t ps4_spawn( const char *path, char **argv, char **envp, int fd0, int fd1 );
extern char **environ;

/* what the child of spawn_process does up to exec_wineloader, done here: the child of ps4_spawn
 * only execs */
static pid_t ps4_spawn_loader( const RTL_USER_PROCESS_PARAMETERS *params, int socketfd, char *winedebug,
                               const struct pe_image_info *pe_info, int stdin_fd, int stdout_fd )
{
    static const char *const replaced[] = { "WINESERVERSOCKET=", "WINEPRELOADRESERVE=", "WINELOADERNOEXEC=", "WINEDEBUG=" };
    ULONGLONG res_start = pe_info->base, res_end = pe_info->base + pe_info->map_size;
    char socket_env[64], reserve_env[64], noexec_env[] = "WINELOADERNOEXEC=1";
    char **argv, **envp;
    int null_fd = -1, count = 0, n = 0, i, k;
    pid_t pid;

    if (pe_info->wine_fakedll) res_start = res_end = 0;
    if ((peb->ProcessParameters && params->ProcessGroupId != peb->ProcessParameters->ProcessGroupId) ||
        params->ConsoleHandle == CONSOLE_HANDLE_ALLOC ||
        params->ConsoleHandle == CONSOLE_HANDLE_ALLOC_NO_WINDOW ||
        params->ConsoleHandle == NULL)
        stdin_fd = stdout_fd = -1;
    if (stdin_fd == -1 || stdout_fd == -1) null_fd = open( "/dev/null", O_RDWR );
    if (stdin_fd == -1) stdin_fd = null_fd;
    if (stdout_fd == -1) stdout_fd = null_fd;

    snprintf( socket_env, sizeof(socket_env), "WINESERVERSOCKET=%u", socketfd );
    snprintf( reserve_env, sizeof(reserve_env), "WINEPRELOADRESERVE=%x%08x-%x%08x",
              (UINT)(res_start >> 32), (UINT)res_start, (UINT)(res_end >> 32), (UINT)res_end );
    while (environ[count]) count++;
    envp = malloc( (count + 5) * sizeof(*envp) );
    for (i = 0; i < count; i++)
    {
        for (k = 0; k < ARRAY_SIZE(replaced); k++)
            if (!strncmp( environ[i], replaced[k], strlen( replaced[k] ))) break;
        if (k == ARRAY_SIZE(replaced) || (k == 3 && !winedebug)) envp[n++] = environ[i];
    }
    envp[n++] = socket_env;
    envp[n++] = reserve_env;
    envp[n++] = noexec_env;
    if (winedebug) envp[n++] = winedebug;
    envp[n] = NULL;

    argv = build_argv( &params->CommandLine, 2 );
    argv[1] = (char *)"/app0/wineps4.bin";  /* the app is the loader (see loader_exec) */
    pid = ps4_spawn( argv[1], argv + 1, envp, stdin_fd, stdout_fd );
    if (pid == -1) MESSAGE( "wineps4: cannot start %s: %s\\n", argv[2] ? argv[2] : "?", strerror( errno ));
    free( argv );
    free( envp );
    if (null_fd != -1) close( null_fd );
    return pid;
}
#endif

static NTSTATUS spawn_process( const RTL_USER_PROCESS_PARAMETERS *params, int socketfd,''')

edit('dlls/ntdll/unix/process.c', '''#ifdef __PS4_WINE__
    /* one fork: an orphaned grandchild can't exec on the PS4 */''', '''#ifdef __PS4_WINE__
    pid = ps4_spawn_loader( params, socketfd, winedebug, pe_info, stdin_fd, stdout_fd );
    goto ps4_spawned;
    /* one fork: an orphaned grandchild can't exec on the PS4 */''')

edit('dlls/ntdll/unix/process.c', '''    /* the child is the new process: reap the ones that ended since */''', '''ps4_spawned:
    /* the child is the new process: reap the ones that ended since */''')
print('ok')
