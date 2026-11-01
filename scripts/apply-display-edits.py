"""WinePS4: the display driver and the desktop's input. Applied once to the Wine tree after
apply-server-stays-edits.py; the PE side of the driver is then built by the PC's Wine
(scripts/build-host-wine.sh), its Unix side into the app (app/CMakeLists.txt).

  - wine-overlay/dlls/wineps4.drv is copied into dlls/ and added to configure.ac (configure is
    made again by autoreconf)
  - server/queue.c includes wine-overlay/server/ps4_input.inc (the desktop's input pipe), and the
    app's wineserver opens it at its start
  - explorer's default graphics driver list is "ps4"

  python3 scripts/apply-display-edits.py ~/wine-ps4-build/src/wine
"""
import os
import shutil
import subprocess
import sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
overlay = os.path.join(here, '..', 'wine-overlay')


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


dst = os.path.join(root, 'dlls', 'wineps4.drv')
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(os.path.join(overlay, 'dlls', 'wineps4.drv'), dst)
# the shared header (the desktop has it too)
shutil.copy(os.path.join(here, '..', 'port', 'include', 'ps4_display.h'), dst)
shutil.copy(os.path.join(overlay, 'server', 'ps4_input.inc'), os.path.join(root, 'server'))
shutil.copy(os.path.join(here, '..', 'port', 'include', 'ps4_display.h'), os.path.join(root, 'server'))

edit('configure.ac', 'WINE_CONFIG_MAKEFILE(dlls/wineps.drv)\n',
     'WINE_CONFIG_MAKEFILE(dlls/wineps.drv)\nWINE_CONFIG_MAKEFILE(dlls/wineps4.drv)\n')

s = open(root + '/server/queue.c').read()
assert '#include "ps4_input.inc"' not in s
open(root + '/server/queue.c', 'w').write(s + '''
#ifdef __PS4_WINE__
#include "ps4_input.inc"
#endif
''')

edit('server/main.c', '''        master_socket_timeout = TIMEOUT_INFINITE;
        open_master_socket();''', '''        extern void ps4_input_init(void);
        master_socket_timeout = TIMEOUT_INFINITE;
        open_master_socket();
        ps4_input_init();''')

edit('programs/explorer/desktop.c', 'static const WCHAR default_driver[] = L"', 'static const WCHAR default_driver[] = L"ps4,')

subprocess.check_call(['autoreconf', '-f'], cwd=root)
print('ok')
