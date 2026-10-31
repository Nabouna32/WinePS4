"""WinePS4: 32-bit programs (Wine's WoW64) on the PS4, applied once to the Wine tree (ps4 branch).

The PS4's kernel is FreeBSD's: its 32-bit code selector is 0x33 and %fs is always 0x13 (its GDT's
32-bit fs entry), as the probe found. 32-bit Windows code finds its TEB through %fs, the PS4's C
library its thread data: the FS base is the 32-bit TEB outside the Unix side, and the C library's
inside it (as on Linux, where the selector changes instead). It changes with
sysarch(AMD64_SET_FSBASE) at every way into and out of the Unix side: the syscall and Unix call
dispatchers, the user callback return, signal handlers. The syscall instruction there may clobber
r8-r10 (FreeBSD 9's fast return path restores only rdi, rsi, rdx), so they are kept around it.

wow64cpu reloads %fs from the 32-bit context when it restores a whole context: it now skips the
load when %fs already holds that selector (always, on the PS4: the load would take the base from
the GDT entry, not the thread's).

  python3 scripts/apply-wow64-edits.py ~/wine-ps4-build/src/wine
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

# where the 32-bit TEB's address is kept for sysarch (a pointer to it is what the call takes)
edit(SIG, r'''    DWORD                 mxcsr;         /* 033c Unix-side mxcsr register */
};''', r'''    DWORD                 mxcsr;         /* 033c Unix-side mxcsr register */
    void                 *wow_fsbase;    /* 0340 PS4: the 32-bit TEB, FS's base outside the Unix side */
};''')
edit(SIG, r'''C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct amd64_thread_data, mxcsr ) == 0x33c );''',
     r'''C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct amd64_thread_data, mxcsr ) == 0x33c );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct amd64_thread_data, wow_fsbase ) == 0x340 );

#ifdef __PS4_WINE__
/* FS's base, with no C library in between (its errno is thread data FS may not reach yet) */
static inline void ps4_set_fsbase( void *const *base )
{
    long ret;
    __asm__ volatile( "syscall" : "=a" (ret) : "a" (165 /* SYS_sysarch */), "D" (129 /* AMD64_SET_FSBASE */),
                      "S" (base) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc" );
}
#endif''')

# signal handlers: the C library's FS on the way in, the 32-bit TEB's on the way out
edit(SIG, r'''#ifdef __PS4_WINE__
    amd64_set_gsbase( get_current_teb() );
#endif
    clear_alignment_flag();''', r'''#ifdef __PS4_WINE__
    amd64_set_gsbase( get_current_teb() );
    if (fs32_sel)
    {
        struct amd64_thread_data *thread_data = (struct amd64_thread_data *)&get_current_teb()->GdiTebBatch;
        if (thread_data->pthread_teb) ps4_set_fsbase( &thread_data->pthread_teb );
    }
#endif
    clear_alignment_flag();''')
edit(SIG, r'''#elif defined __APPLE__
    if (!is_inside_signal_stack( (void *)RSP_sig(sigcontext )) &&
        !is_inside_syscall( RSP_sig(sigcontext )))
        _thread_set_tsd_base( (uint64_t)NtCurrentTeb() );
#endif
    if (is_16bit( sigcontext )) return;''', r'''#elif defined __APPLE__
    if (!is_inside_signal_stack( (void *)RSP_sig(sigcontext )) &&
        !is_inside_syscall( RSP_sig(sigcontext )))
        _thread_set_tsd_base( (uint64_t)NtCurrentTeb() );
#elif defined __PS4_WINE__
    if (fs32_sel &&
        !is_inside_signal_stack( (void *)RSP_sig(sigcontext )) &&
        !is_inside_syscall( RSP_sig(sigcontext )))
    {
        struct amd64_thread_data *thread_data = (struct amd64_thread_data *)&get_current_teb()->GdiTebBatch;
        if (thread_data->wow_fsbase) ps4_set_fsbase( &thread_data->wow_fsbase );
    }
#endif
    if (is_16bit( sigcontext )) return;''')

# the selectors
edit(SIG, r'''#ifdef __linux__
        cs32_sel = 0x23;
        fs32_sel = alloc_fs_sel( -1, wow_teb );
#elif defined(__APPLE__)''', r'''#ifdef __linux__
        cs32_sel = 0x23;
        fs32_sel = alloc_fs_sel( -1, wow_teb );
#elif defined(__PS4_WINE__)
        cs32_sel = 0x33;  /* the GDT's 32-bit user code */
        fs32_sel = 0x13;  /* its 32-bit fs entry: what %fs always holds; the base is set (sysarch) */
#elif defined(__APPLE__)''')

# a thread's start: both FS bases
edit(SIG, r'''#elif defined(__PS4_WINE__)
    amd64_set_gsbase( teb );
    ps4_thread_teb = teb;
    ps4_thread_register();''', r'''#elif defined(__PS4_WINE__)
    amd64_set_gsbase( teb );
    ps4_thread_teb = teb;
    ps4_thread_register();
    if (fs32_sel && get_wow_teb( teb ))
    {
        amd64_get_fsbase( &thread_data->pthread_teb );
        thread_data->wow_fsbase = get_wow_teb( teb );
    }''')

# --- the dispatchers --------------------------------------------------------------------------
# the way in (syscalls): the C library's FS; r8-r10 hold arguments
edit(SIG, r'''                   "mov $158,%eax\n\t"             /* SYS_arch_prctl */
                   "syscall\n\t"
                   "leaq -0x98(%rbp),%rcx\n"
                   "2:\n\t"
#elif defined __APPLE__''', r'''                   "mov $158,%eax\n\t"             /* SYS_arch_prctl */
                   "syscall\n\t"
                   "leaq -0x98(%rbp),%rcx\n"
                   "2:\n\t"
#elif defined __PS4_WINE__
                   "cmpq $0,0x320(%r13)\n\t"       /* amd64_thread_data()->pthread_teb: 32-bit programs */
                   "jz 2f\n\t"
                   "pushq %r8\n\t"
                   "pushq %r9\n\t"
                   "pushq %r10\n\t"
                   "movl $129,%edi\n\t"            /* AMD64_SET_FSBASE */
                   "leaq 0x320(%r13),%rsi\n\t"
                   "movl $165,%eax\n\t"            /* SYS_sysarch */
                   "syscall\n\t"
                   "popq %r10\n\t"
                   "popq %r9\n\t"
                   "popq %r8\n\t"
                   "leaq -0x98(%rbp),%rcx\n"
                   "2:\n\t"
#elif defined __APPLE__''')
# the way in (Unix calls): r8 (args), r10 (table) and rdx (code) live
edit(SIG, r'''                   "mov $158,%eax\n\t"             /* SYS_arch_prctl */
                   "syscall\n\t"
                   "2:\n\t"
#elif defined __APPLE__''', r'''                   "mov $158,%eax\n\t"             /* SYS_arch_prctl */
                   "syscall\n\t"
                   "2:\n\t"
#elif defined __PS4_WINE__
                   "cmpq $0,0x320(%r13)\n\t"       /* amd64_thread_data()->pthread_teb: 32-bit programs */
                   "jz 2f\n\t"
                   "pushq %r8\n\t"
                   "pushq %r10\n\t"
                   "pushq %rdx\n\t"
                   "movl $129,%edi\n\t"            /* AMD64_SET_FSBASE */
                   "leaq 0x320(%r13),%rsi\n\t"
                   "movl $165,%eax\n\t"            /* SYS_sysarch */
                   "syscall\n\t"
                   "popq %rdx\n\t"
                   "popq %r10\n\t"
                   "popq %r8\n"
                   "2:\n\t"
#elif defined __APPLE__''')
# the way out of a syscall: rax (status) and rcx (frame) live; r14 and r15 are restored after
edit(SIG, r'''#elif defined __APPLE__
                   "movq %rax,%r8\n\t"
                   "movq %rcx,%rdx\n\t"
                   "movq %r13,%rdi\n\t"            /* teb */
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"      /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %rdx,%rcx\n\t"
                   "movq %r8,%rax\n\t"
#endif''', r'''#elif defined __APPLE__
                   "movq %rax,%r8\n\t"
                   "movq %rcx,%rdx\n\t"
                   "movq %r13,%rdi\n\t"            /* teb */
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"      /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %rdx,%rcx\n\t"
                   "movq %r8,%rax\n\t"
#elif defined __PS4_WINE__
                   "cmpq $0,0x340(%r13)\n\t"       /* amd64_thread_data()->wow_fsbase: 32-bit programs */
                   "jz 1f\n\t"
                   "movq %rax,%r14\n\t"
                   "movq %rcx,%r15\n\t"
                   "movl $129,%edi\n\t"            /* AMD64_SET_FSBASE */
                   "leaq 0x340(%r13),%rsi\n\t"
                   "movl $165,%eax\n\t"            /* SYS_sysarch */
                   "syscall\n\t"
                   "movq %r15,%rcx\n\t"
                   "movq %r14,%rax\n"
                   "1:\n\t"
#endif''')
# the way out of a Unix call: rax and rcx live; r13 and r14 are restored after
edit(SIG, r'''#elif defined __APPLE__
                   "movq %rax,%rdx\n\t"
                   "movq %rcx,%r14\n\t"
                   "movq %r13,%rdi\n\t"            /* teb */
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"      /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %r14,%rcx\n\t"
                   "movq %rdx,%rax\n\t"
                   "movq 0x60(%rcx),%r14\n\t"
#endif''', r'''#elif defined __APPLE__
                   "movq %rax,%rdx\n\t"
                   "movq %rcx,%r14\n\t"
                   "movq %r13,%rdi\n\t"            /* teb */
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"      /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %r14,%rcx\n\t"
                   "movq %rdx,%rax\n\t"
                   "movq 0x60(%rcx),%r14\n\t"
#elif defined __PS4_WINE__
                   "cmpq $0,0x340(%r13)\n\t"       /* amd64_thread_data()->wow_fsbase: 32-bit programs */
                   "jz 1f\n\t"
                   "leaq 0x340(%r13),%rsi\n\t"
                   "movq %rax,%r13\n\t"
                   "movq %rcx,%r14\n\t"
                   "movl $129,%edi\n\t"            /* AMD64_SET_FSBASE */
                   "movl $165,%eax\n\t"            /* SYS_sysarch */
                   "syscall\n\t"
                   "movq %r14,%rcx\n\t"
                   "movq %r13,%rax\n\t"
                   "movq 0x60(%rcx),%r14\n"
                   "1:\n\t"
#endif''')
# the way out to a user callback: rcx (the function) live; r15 is cleared after
edit(SIG, r'''#elif defined __APPLE__
                   "movq %rcx,%r10\n\t"
                   "movq %r13,%rdi\n\t"
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"  /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %r10,%rcx\n\t"
#endif''', r'''#elif defined __APPLE__
                   "movq %rcx,%r10\n\t"
                   "movq %r13,%rdi\n\t"
                   "xorl %esi,%esi\n\t"
                   "movl $0x3000003,%eax\n\t"  /* _thread_set_tsd_base */
                   "syscall\n\t"
                   "movq %r10,%rcx\n\t"
#elif defined __PS4_WINE__
                   "cmpq $0,0x340(%r13)\n\t"   /* amd64_thread_data()->wow_fsbase: 32-bit programs */
                   "jz 1f\n\t"
                   "movq %rcx,%r15\n\t"
                   "movl $129,%edi\n\t"        /* AMD64_SET_FSBASE */
                   "leaq 0x340(%r13),%rsi\n\t"
                   "movl $165,%eax\n\t"        /* SYS_sysarch */
                   "syscall\n\t"
                   "movq %r15,%rcx\n"
                   "1:\n\t"
#endif''')

# wow64cpu: %fs reloaded only when it isn't the selector already
edit('dlls/wow64cpu/cpu.c', r'''                   "mov 0x90(%r13),%fs\n\t"     /* context->SegFs */''',
     r'''                   "movw %fs,%ax\n\t"
                   "cmpw 0x90(%r13),%ax\n\t"    /* context->SegFs */
                   "je 1f\n\t"                  /* (the PS4: a load would take the GDT entry's base) */
                   "mov 0x90(%r13),%fs\n"
                   "1:\n\t"''')
print('ok')
