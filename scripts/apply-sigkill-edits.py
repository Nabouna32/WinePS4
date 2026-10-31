"""WinePS4: wineserver never kills the app, applied once to the Wine tree after the other edits.

When a Windows process ends, wineserver waits for its Unix process to go (kill( pid, 0 )), then
sends SIGKILL. Here the Windows process is a thread of the app, and the Unix process is the app,
desktop and all: it was killed half a second after every program ended (v00.16, v00.17: back to the
PS4 menu, no report). A process in this one is gone once it says so.

  python3 scripts/apply-sigkill-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]
p = root + '/server/process.c'
s = open(p).read()
old = '''    struct process *process = private;
    int signal = 0;

    process->sigkill_delay *= 2;'''
new = '''    struct process *process = private;
    int signal = 0;

#ifdef __PS4_WINE__
    /* the program ran in this process (a thread of it): nothing to wait for, nothing to kill */
    if (process->unix_pid == getpid())
    {
        process->sigkill_timeout = NULL;
        process_died( process );
        return;
    }
#endif
    process->sigkill_delay *= 2;'''
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, new))
print('ok')
