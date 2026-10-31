"""WinePS4: Wine's signal handlers installed with the kernel's sigaction, applied once to the Wine
tree (ps4 branch), after apply-wow64-edits.py.

The PS4's C library (FreeBSD's libthr) wraps every handler sigaction installs in one of its own,
which finds its thread data through %fs. In 32-bit code %fs's base is the 32-bit TEB
(apply-wow64-edits.py), so that wrapper crashed before Wine's handler ran, and the kernel ended the
app with no report (v00.15: the first fault in 32-bit code). Installed with the system call, the
handler is called by the kernel's trampoline directly, and init_handler puts the C library's FS
back first.

  python3 scripts/apply-signal-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


SIG = 'dlls/ntdll/unix/signal_x86_64.c'

edit(SIG, r'''#ifdef __PS4_WINE__
/* FS's base, with no C library in between (its errno is thread data FS may not reach yet) */''',
     r'''#ifdef __PS4_WINE__
/* the handlers, without the C library's wrapper (it needs FS: ps4_wine_port.c) */
extern int ps4_raw_sigaction( int sig, const struct sigaction *act, struct sigaction *old );
#define sigaction( sig, act, old ) ps4_raw_sigaction( sig, act, old )

/* FS's base, with no C library in between (its errno is thread data FS may not reach yet) */''')

# init_handler: the C library's FS before anything else (amd64_set_gsbase is a C library call)
edit(SIG, r'''#ifdef __PS4_WINE__
    amd64_set_gsbase( get_current_teb() );
    if (fs32_sel)
    {
        struct amd64_thread_data *thread_data = (struct amd64_thread_data *)&get_current_teb()->GdiTebBatch;
        if (thread_data->pthread_teb) ps4_set_fsbase( &thread_data->pthread_teb );
    }
#endif''', r'''#ifdef __PS4_WINE__
    if (fs32_sel)
    {
        struct amd64_thread_data *thread_data = (struct amd64_thread_data *)&get_current_teb()->GdiTebBatch;
        if (thread_data->pthread_teb) ps4_set_fsbase( &thread_data->pthread_teb );
    }
    amd64_set_gsbase( get_current_teb() );
#endif''')
print('ok')
