"""WinePS4: Wine's startup (wineboot, which starts services.exe) runs once per app start. Applied
once to the Wine tree after apply-spawn-heap-edits.py.

v00.33: every program skipped wineboot, so services.exe never ran and nothing answered on the
svcctl and rpcss pipes (start_rpcss: "Failed to open service manager"; installers need them). The
desktop starts wineboot.exe --init itself (WINEPS4_BOOT=1 in its environment: it must not run
another wineboot). wineboot holds the boot event and services.exe inherits it, so the event stays
while services.exe runs: the programs started later find it set and go on, as on the PC.

  python3 scripts/apply-boot-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


edit('dlls/ntdll/unix/env.c', '''#ifdef __PS4_WINE__
    /* the package's prefix is set up already (the PC's wineboot made it); the shared data
     * page (SystemCall = 1 and the cpu info) is filled here, before PE code runs */
    virtual_init_user_shared_data();
    return;
#endif''', '''#ifdef __PS4_WINE__
    /* the shared data page (SystemCall = 1 and the cpu info) is filled here, before PE code runs,
     * also when the boot is done already. The desktop starts wineboot itself (WINEPS4_BOOT): that
     * one doesn't start another. */
    virtual_init_user_shared_data();
    if (getenv( "WINEPS4_BOOT" )) return;
#endif''')

# (not passed on: the processes wineboot starts have a parent, but the ones they start must not
# skip a boot either)
edit('dlls/ntdll/unix/process.c', '''"WINELOADERNOEXEC=", "WINEDEBUG=" };''',
     '''"WINELOADERNOEXEC=", "WINEDEBUG=", "WINEPS4_BOOT=" };''')
print('ok')
