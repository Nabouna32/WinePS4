#!/usr/bin/env python3
"""One-time edits that started the ps4 branch of Wine (~/wine-ps4-build/src/wine). Kept for the record;
the branch itself (patches/wine-ps4.patch) is the source of truth from then on."""
import sys, pathlib
W = pathlib.Path(sys.argv[1])

def edit(rel, old, new, count=1):
    p = W / rel
    s = p.read_text()
    n = s.count(old)
    if n != count:
        sys.exit(f"{rel}: expected {count} of {old!r}, found {n}")
    p.write_text(s.replace(old, new))

FBSD = "defined(__FreeBSD__) || defined(__FreeBSD_kernel__)"
sig = "dlls/ntdll/unix/signal_x86_64.c"
edit(sig, "#elif defined(__FreeBSD__) || defined (__FreeBSD_kernel__)\n\n#include <machine/trap.h>",
     "#elif defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__PS4_WINE__)\n\n#include <machine/trap.h>")
edit(sig, "#if defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)",
     "#if defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__PS4_WINE__)")
edit(sig, "#elif " + FBSD + "\n    amd64_get_gsbase", "#elif " + FBSD + " || defined(__PS4_WINE__)\n    amd64_get_gsbase")
edit(sig, "#elif " + FBSD + "\n    amd64_set_gsbase( teb );\n#elif defined(__NetBSD__)\n    sysarch( X86_64_SET_GSBASE, &teb );\n#elif defined(__APPLE__)\n    /* leave_handler",
     "#elif " + FBSD + " || defined(__PS4_WINE__)\n    amd64_set_gsbase( teb );\n#elif defined(__NetBSD__)\n    sysarch( X86_64_SET_GSBASE, &teb );\n#elif defined(__APPLE__)\n    /* leave_handler")
edit(sig, "#elif defined (__FreeBSD__) || defined (__FreeBSD_kernel__)\n    amd64_set_gsbase( teb );",
     "#elif defined (__FreeBSD__) || defined (__FreeBSD_kernel__)\n    amd64_set_gsbase( teb );\n#elif defined(__PS4_WINE__)\n    amd64_set_gsbase( teb );\n    ps4_thread_teb = teb;\n    ps4_thread_register();")
# In a signal handler the PS4 does not give the thread's GS base back: find the TEB from the signal
# stack (it is inside the TEB block, as on Linux and macOS) and put it back in GS.
edit(sig, "#if defined(__linux__) || defined(__APPLE__)\nstatic inline TEB *get_current_teb(void)",
     "#if defined(__linux__) || defined(__APPLE__) || defined(__PS4_WINE__)\nstatic inline TEB *get_current_teb(void)")
edit(sig, "static inline ucontext_t *init_handler( void *sigcontext )\n{\n    clear_alignment_flag();\n",
     "static inline ucontext_t *init_handler( void *sigcontext )\n{\n#ifdef __PS4_WINE__\n    amd64_set_gsbase( get_current_teb() );\n#endif\n    clear_alignment_flag();\n")

virt = "dlls/ntdll/unix/virtual.c"
# 16 KiB host pages: Wine's large-page support (written for ARM64) merges the protections of the
# 4 KiB Windows pages in each host page.
edit(virt, "#ifdef __aarch64__\nstatic UINT_PTR host_page_size;", "#if defined(__aarch64__) || defined(__PS4_WINE__)\nstatic UINT_PTR host_page_size;")
edit(virt, "#ifdef __aarch64__\n    host_page_size = sysconf( _SC_PAGESIZE );", "#if defined(__aarch64__) || defined(__PS4_WINE__)\n    host_page_size = sysconf( _SC_PAGESIZE );")

ld = "dlls/ntdll/unix/loader.c"
# No dladdr for a statically linked eboot: the packaged layout is fixed.
LOOKUP = r'''    Dl_info info;

    if (!dladdr( init_paths, &info ) || !(ntdll_dir = realpath_dirname( info.dli_fname )))
        fatal_error( "cannot get path to ntdll.so\n" );
'''
edit(ld, LOOKUP, "#ifdef __PS4_WINE__\n    ntdll_dir = strdup( LIBDIR \"/wine/x86_64-unix\" );\n#else\n" + LOOKUP + "#endif\n")

srv = "dlls/ntdll/unix/server.c"
# wineserver is a thread of this process, reached through a socket pair (port/ps4_wine_port.c).
edit(srv, "static int server_connect(void)\n{\n",
     "#ifdef __PS4_WINE__\nextern int ps4_start_wineserver( void );\n#endif\nstatic int server_connect(void)\n{\n#ifdef __PS4_WINE__\n    return ps4_start_wineserver();\n#endif\n")
edit(srv, "#ifdef __ANDROID__  /* there's no /tmp dir on Android */", "#if defined(__ANDROID__) || defined(__PS4_WINE__)  /* there's no /tmp dir on Android */")

edit("server/request.c", "#ifdef __ANDROID__  /* there's no /tmp dir on Android */", "#if defined(__ANDROID__) || defined(__PS4_WINE__)  /* there's no /tmp dir on Android */")
edit("server/ptrace.c", "#elif (defined(__FreeBSD__) || defined (__FreeBSD_kernel__)) && defined(HAVE_THR_KILL2)\n    return thr_kill2( tgid, pid, sig );",
     "#elif (defined(__FreeBSD__) || defined (__FreeBSD_kernel__)) && defined(HAVE_THR_KILL2)\n    return thr_kill2( tgid, pid, sig );\n#elif defined(__PS4_WINE__)\n    return ps4_thread_kill( pid, sig );")
print("edits applied")

# wineserver as a thread: no master socket, no lock, no fork; the one client arrives as a socket pair.
edit("server/request.c", "/* master socket timer expiration handler */",
     """#ifdef __PS4_WINE__
/* WinePS4: the server directory without the master socket or the lock (one client, this process). */
void ps4_server_dir_init(void)
{
    server_dir = create_server_dir( 1 );
    init_tracing_mechanism();
}
#endif

/* master socket timer expiration handler */""")
edit("server/main.c", "int main( int argc, char *argv[] )\n{",
     """#ifdef __PS4_WINE__
extern void ps4_server_dir_init(void);

/* WinePS4: wineserver as a thread of the app's process. Its signals belong to ntdll in this process,
 * so init_signals() is left out (it only handles SIGCHLD/SIGHUP/SIGINT/SIGTERM/SIGIO for a server
 * of its own). */
int ps4_wineserver_main( int client_fd )
{
    server_argv0 = "wineserver";
    foreground = 1;
    if (getenv( "WINEPS4_SERVER_DEBUG" )) debug_level = atoi( getenv( "WINEPS4_SERVER_DEBUG" ));
    init_limits();
    sock_init();
    ps4_server_dir_init();
    if (debug_level) fprintf( stderr, "wineserver: starting in-process\n" );
    set_current_time();
    init_memory();
    init_directories( load_intl_file() );
    init_threading();
    init_registry();
    if (!create_process( client_fd, NULL, 0, NULL, NULL, NULL, 0, NULL ))
        fatal_error( "wineserver: could not create the first process\n" );
    main_loop();
    return 0;
}
#endif

int main( int argc, char *argv[] )
{""")
print("server edits applied")
