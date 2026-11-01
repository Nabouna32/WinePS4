"""WinePS4: the app's wineserver never closes, and programs don't run wineboot. Applied once to the
Wine tree after apply-multiprocess-edits.py.

v00.21: wineserver closed its socket and exited (0) once a program ended, though it never times out
(the cause unknown yet: the caller is logged now), and the program it was talking to was killed
(SIGKILL). The app's server serves for as long as the app runs.

wineboot (run_wineboot, at every program's start when the prefix is new) is a process of its own
that sets the prefix up: the package's prefix is set up already (the PC's wineboot made it).
v00.22: wineboot is also where the shared data page gets filled (SystemCall = 1): without it every
PE system call faulted (heap handle 0, endless exceptions), so programs fill it themselves (v00.23).

  python3 scripts/apply-server-stays-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


edit('server/request.c', '''void close_master_socket( timeout_t timeout )
{
    if (master_socket)''', '''void close_master_socket( timeout_t timeout )
{
#ifdef __PS4_WINE__
    /* the app's server serves for as long as the app runs */
    fprintf( stderr, "wineserver: close_master_socket(%lld) from %p ignored\\n", (long long)timeout,
             __builtin_return_address( 0 ));
    return;
#endif
    if (master_socket)''')

edit('dlls/ntdll/unix/env.c', '''static void run_wineboot( WCHAR *env, SIZE_T size )
{''', '''static void run_wineboot( WCHAR *env, SIZE_T size )
{
#ifdef __PS4_WINE__
    /* the package's prefix is set up already (the PC's wineboot made it); the shared data
     * page (SystemCall = 1 and the cpu info) is filled here, before PE code runs */
    virtual_init_user_shared_data();
    return;
#endif''')
print('ok')
