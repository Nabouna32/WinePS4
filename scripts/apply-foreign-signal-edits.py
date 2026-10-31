"""WinePS4: Wine's signal handlers hand signals of threads that aren't Wine's to what was there
before (ps4-kit's crash handler), applied once to the Wine tree after apply-signal-edits.py.

The desktop and ps4-kit's threads share the process: a crash there got Wine's handler, which looked
for a TEB at the bottom of a signal stack that isn't Wine's, and the app ended with no report.
ps4_foreign_signal (port/ps4_wine_port.c) knows Wine's TEBs; it is asked first, with the TEB the
signal stack would belong to (only arithmetic: %fs may be the 32-bit TEB's).

  python3 scripts/apply-foreign-signal-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]
SIG = root + '/dlls/ntdll/unix/signal_x86_64.c'
s = open(SIG).read()

decl = r'''#define sigaction( sig, act, old ) ps4_raw_sigaction( sig, act, old )'''
assert s.count(decl) == 1
s = s.replace(decl, decl + r'''
extern int ps4_foreign_signal( void *teb, int sig, siginfo_t *info, void *context );''')

check = r'''
#ifdef __PS4_WINE__
    if (ps4_foreign_signal( get_current_teb(), signal, siginfo, sigcontext )) return;
#endif
'''
for name in ('segv', 'trap', 'fpe', 'int', 'abrt', 'quit', 'usr1'):
    old = ('static void %s_handler( int signal, siginfo_t *siginfo, void *sigcontext )\n{\n'
           '    ucontext_t *ucontext = init_handler( sigcontext );\n') % name
    new = ('static void %s_handler( int signal, siginfo_t *siginfo, void *sigcontext )\n{\n'
           '    ucontext_t *ucontext;\n' + check + '    ucontext = init_handler( sigcontext );\n') % name
    assert s.count(old) == 1, name
    s = s.replace(old, new)

old = ('static void sigsys_handler( int signal, siginfo_t *siginfo, void *sigcontext )\n{\n'
       '    extern const void *__wine_syscall_dispatcher_prolog_end_ptr;\n'
       '    ucontext_t *ucontext = init_handler( sigcontext );\n')
new = ('static void sigsys_handler( int signal, siginfo_t *siginfo, void *sigcontext )\n{\n'
       '    extern const void *__wine_syscall_dispatcher_prolog_end_ptr;\n'
       '    ucontext_t *ucontext;\n' + check + '    ucontext = init_handler( sigcontext );\n')
assert s.count(old) == 1, 'sigsys'
s = s.replace(old, new)
open(SIG, 'w').write(s)
print('ok')
