"""WinePS4: programs start other programs with one fork. Applied once to the Wine tree after
apply-display-edits.py.

Wine's spawn_process forks twice so the new process has no parent to reap it: the middle one exits
at once and the grandchild execs the loader. On the PS4 that orphan's execve of /app0/wineps4.bin
failed (exit 1: explorer.exe never started, so no program had a desktop window, v00.27). The child
execs instead, and is reaped by the program that started it (a zombie until then, or until that
program ends). loader_exec says why an exec fails.

  python3 scripts/apply-spawn-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


edit('dlls/ntdll/unix/process.c', '''    if (!(pid = fork()))  /* child */
    {
        if (!(pid = fork()))  /* grandchild */
        {''', '''#ifdef __PS4_WINE__
    /* one fork: an orphaned grandchild can't exec on the PS4 */
    if (!(pid = fork()))  /* child */
    {
        if (1)
        {
#else
    if (!(pid = fork()))  /* child */
    {
        if (!(pid = fork()))  /* grandchild */
        {
#endif''')

edit('dlls/ntdll/unix/process.c', '''    if (pid != -1)
    {
        /* reap child */
        pid_t wret;
        do {
            wret = waitpid(pid, NULL, 0);
        } while (wret < 0 && errno == EINTR);
    }
    else status = STATUS_NO_MEMORY;''', '''#ifdef __PS4_WINE__
    /* the child is the new process: reap the ones that ended since */
    if (pid == -1) status = STATUS_NO_MEMORY;
    while (waitpid( -1, NULL, WNOHANG ) > 0) ;
#else
    if (pid != -1)
    {
        /* reap child */
        pid_t wret;
        do {
            wret = waitpid(pid, NULL, 0);
        } while (wret < 0 && errno == EINTR);
    }
    else status = STATUS_NO_MEMORY;
#endif''')

edit('dlls/ntdll/unix/loader.c', '''    argv[1] = (char *)"/app0/wineps4.bin";
    execv( argv[1], argv + 1 );
    return STATUS_INVALID_IMAGE_FORMAT;''', '''    argv[1] = (char *)"/app0/wineps4.bin";
    execv( argv[1], argv + 1 );
    MESSAGE( "wineps4: exec %s for %s failed: %s\\n", argv[1], argv[2] ? argv[2] : "?", strerror( errno ));
    return STATUS_INVALID_IMAGE_FORMAT;''')
print('ok')
