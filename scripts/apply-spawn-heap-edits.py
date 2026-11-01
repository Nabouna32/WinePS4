"""WinePS4: the new process's environment strings in malloc'd memory. Applied once to the Wine tree
after apply-spawn-thread-edits.py.

v00.30: the child of a program's fork read strings on the Wine thread's stack (direct memory, which a
forked child does not have) and died before its exec. ps4_spawn (port/ps4_wine_port.c) uses vfork
now, but its child must still only read malloc'd memory in case the PS4 gives it a copy.

  python3 scripts/apply-spawn-heap-edits.py ~/wine-ps4-build/src/wine
"""
import sys

root = sys.argv[1]


def edit(path, old, new, count=1):
    p = root + '/' + path
    s = open(p).read()
    n = s.count(old)
    assert n == count, '%s: %d matches for %r' % (path, n, old[:70])
    open(p, 'w').write(s.replace(old, new))


edit('dlls/ntdll/unix/process.c', '''    envp[n++] = socket_env;
    envp[n++] = reserve_env;
    envp[n++] = noexec_env;
    if (winedebug) envp[n++] = winedebug;
    envp[n] = NULL;''', '''    /* (the child reads only malloc'd memory: see ps4_spawn) */
    k = n;  /* the ones made here, freed below */
    envp[n++] = strdup( socket_env );
    envp[n++] = strdup( reserve_env );
    envp[n++] = strdup( noexec_env );
    if (winedebug) envp[n++] = strdup( winedebug );
    envp[n] = NULL;''')

edit('dlls/ntdll/unix/process.c', '''    free( argv );
    free( envp );
    if (null_fd != -1) close( null_fd );
    return pid;''', '''    free( argv );
    for (i = k; envp[i]; i++) free( envp[i] );  /* the ones made above (the others are environ's) */
    free( envp );
    if (null_fd != -1) close( null_fd );
    return pid;''')
print('ok')
