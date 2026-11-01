"""WinePS4: where a program's child process stops before it runs. Applied once to the Wine tree after
apply-spawn-edits.py.

v00.28: explorer.exe, started by every program for the desktop window, ended with 1 before the app
ran in it, without an exec error. The child writes a line at each step (plain write(2): nothing that
takes a lock another thread of the parent could have held at the fork), and the parent logs how the
child ended when the new process does not start.

  python3 scripts/apply-spawn-trace-edits.py ~/wine-ps4-build/src/wine
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
static pid_t ps4_spawn_pid;  /* the last child, for NtCreateUserProcess's report when it fails */

static void ps4_spawn_step( const char *step )
{
    char line[128];
    int len = snprintf( line, sizeof(line), "wineps4: child %d: %s\\n", (int)getpid(), step );
    write( 2, line, len );
}
#endif

static NTSTATUS spawn_process( const RTL_USER_PROCESS_PARAMETERS *params, int socketfd,''')

edit('dlls/ntdll/unix/process.c', '''        if (1)
        {
#else''', '''        if (1)
        {
            ps4_spawn_step( "forked" );
#else''')

edit('dlls/ntdll/unix/process.c', '''            argv = build_argv( &params->CommandLine, 2 );

            exec_wineloader( argv, socketfd, pe_info );''', '''            argv = build_argv( &params->CommandLine, 2 );

#ifdef __PS4_WINE__
            ps4_spawn_step( "exec" );
#endif
            exec_wineloader( argv, socketfd, pe_info );''')

edit('dlls/ntdll/unix/process.c', '''    /* the child is the new process: reap the ones that ended since */
    if (pid == -1) status = STATUS_NO_MEMORY;''', '''    /* the child is the new process: reap the ones that ended since */
    if (pid == -1) status = STATUS_NO_MEMORY;
    ps4_spawn_pid = pid;''')

edit('dlls/ntdll/unix/process.c', '''    if (!success)
    {
        if (!status) status = STATUS_INTERNAL_ERROR;''', '''    if (!success)
    {
#ifdef __PS4_WINE__
        {
            int wstatus = 0;
            pid_t got = waitpid( ps4_spawn_pid, &wstatus, WNOHANG );
            MESSAGE( "wineps4: %s did not start (exit code %#x): child %d %s %d\\n", debugstr_us(&path), status,
                     (int)ps4_spawn_pid, got <= 0 ? "still there, status" :
                     WIFSIGNALED( wstatus ) ? "killed by signal" : "exited with",
                     got <= 0 ? 0 : WIFSIGNALED( wstatus ) ? WTERMSIG( wstatus ) : WEXITSTATUS( wstatus ));
        }
#endif
        if (!status) status = STATUS_INTERNAL_ERROR;''')
print('ok')
