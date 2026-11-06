/* WinePS4: where the kernel has the app's threads, read without a signal: the freeze report's other
 * half. v00.64's thread dump had two of the desktop's threads that never answered its signal, then
 * stood itself on the next one: they waited in the kernel, where a signal waits until they return.
 * The kernel's record of a thread tells its state and what it waits on: its sleep's message (umtxn a
 * mutex, select, accept, ufs a file's lock, pfault memory, biord the disk, ...) or the kernel lock it
 * is blocked on, and for how long. The PS4 answers kern.proc.pid with threads with ENOENT (v00.65)
 * and its records are 1096 bytes, not FreeBSD 9's 1088 (v00.67's look at the forms), so the report
 * reads every thread of the system from kern.proc.all, with the offsets found in this thread's own
 * record as the desktop starts (calibrate).
 *
 * The desktop asks at a freeze (port/ps4_main.c, on a thread of its own: the report's signals and
 * this don't wait for each other). Asking wires the asker's buffer, which takes its address space's
 * lock, so a desktop whose address space is what stands can't ask. Threads of their own touch memory
 * of each kind the desktop has at the same time (the memory probe): v00.69's freeze watch stood in a
 * query of the app's direct memory, as the desktop's loop and wineserver had in their work. All of it
 * without printf, malloc or a lock a stuck thread may hold. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "include/ps4_line.h"
#include "include/ps4_prio.h"

/* FreeBSD 9's struct kinfo_proc for amd64 (sys/user.h): where what is read of it is, in a record of
 * kern.proc. This kernel's own replace them as the desktop starts (calibrate): the PS4's records are
 * 8 bytes longer. */
static struct
{
    int size, wchan, pid, slptime, runtime, kiflag, stat, oncpu, tdname, wmesg, lockname, tid;
} kl = { 1088, 64, 72, 316, 328, 376, 388, 392, 394, 411, 438, 600 };
#define KI_LOCKBLOCK   0x4   /* (ki_kiflag) blocked on a kernel lock: ki_lockname says which */
#define KINFO_BUFFER   (2304u << 10)  /* kern.proc.all: a record a thread of the system, ~2100 of them */

#define HEARTBEAT_FILE  "/data/WinePS4/.heartbeat"
#define HEARTBEAT_MAGIC 0x57504842  /* "WPHB" */
#define HEARTBEAT_SIZE  16384
#define MAX_PIDS        64
#define WATCH_SECONDS   10

struct heartbeat
{
    uint32_t magic;
    int32_t desktop_pid;
    uint32_t beats;     /* the desktop's loop (ps4_desktop_beat) */
    uint32_t requests;  /* wineserver's requests so far, each second (the freeze watch) */
    uint32_t told;      /* the beats at which the desktop told its stand (0: not yet) */
    int32_t watcher;    /* the program that tells the desktop's stand (0: none) */
    int32_t pids[MAX_PIDS];       /* the app's program processes (0: a free place) */
    char names[MAX_PIDS][32];     /* their programs' file names */
};

/* kern.proc's records: KINFO_BUFFER of flexible memory the desktop maps (ps4_kstate_prepare), not
 * of its heap's direct memory, which is what may stand at a freeze */
static unsigned char *kinfo;
static size_t kinfo_size;
static struct heartbeat *heartbeat;
static int is_desktop;

int sceKernelAvailableFlexibleMemorySize( size_t *size );
int32_t sceKernelMapFlexibleMemory( void **addr, size_t len, int prot, int flags );
int pthread_getthreadid_np( void );
static uint64_t flexible_free( void );
static void memory_probe_start( void );
static void memory_probe_wake( void );
static void memory_probe_report( void );
extern void ps4_thread_table_name( const char *name );  /* (port/ps4_wine_port.c) */

/* where a summary line goes: the desktop's wine.log, written straight (port/ps4_main.c); a program's
 * standard error (the desktop's log pipe) when NULL */
void (*ps4_kstate_log)( const char *line );

static long kstate_syscall3( long number, long a, long b, long c )
{
    long ret;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "=a" (ret), "=qm" (failed)
                      : "a" (number), "D" (a), "S" (b), "d" (c)
                      : "rcx", "r8", "r9", "r10", "r11", "memory", "cc" );
    return failed ? -ret : ret;
}

static long kstate_sysctl( int *mib, unsigned int len, void *old, size_t *oldlen )
{
    register long r10 __asm__("r10") = (long)oldlen;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    long rax = 202 /* SYS___sysctl */, rdx = (long)old;
    unsigned char failed;

    __asm__ volatile( "syscall\n\tsetc %1"
                      : "+a" (rax), "=qm" (failed), "+d" (rdx), "+r" (r10), "+r" (r8), "+r" (r9)
                      : "D" ((long)mib), "S" ((long)len)
                      : "rcx", "r11", "memory", "cc" );
    return failed ? -rax : 0;
}

static void put_text( long fd, const struct ps4_line *l )
{
    if (fd >= 0) kstate_syscall3( 4 /* SYS_write */, fd, (long)l->text, l->len );
}

/* a kernel string of at most max bytes, printable characters only */
static void put_name( struct ps4_line *l, const unsigned char *s, int max )
{
    int i;

    for (i = 0; i < max && s[i]; i++)
        if (l->len < (int)sizeof(l->text) - 1) l->text[l->len++] = s[i] >= 0x20 && s[i] < 0x7f ? (char)s[i] : '?';
}

static void log_line( struct ps4_line *l )
{
    l->text[l->len] = 0;
    if (ps4_kstate_log) ps4_kstate_log( l->text );
    else
    {
        ps4_line_str( l, "\n" );
        kstate_syscall3( 4 /* SYS_write */, 2, (long)l->text, l->len );
    }
}

/* Which forms of kern.proc this kernel answers, told once (v00.65: kern.proc.pid with threads gave
 * ENOENT for every process): with threads (17), the process alone (1), every process (0, 8). */
static void probe_kern_proc( int pid )
{
    static int done;
    static const struct { int number, mib_len; const char *name; } forms[] =
    {
        { 1 | 0x10, 4, "pid_td" }, { 1, 4, "pid" }, { 0, 3, "all" }, { 8, 3, "proc" },
    };
    struct ps4_line l;
    unsigned int i;

    if (done || !kinfo) return;
    done = 1;
    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: kern.proc's forms here:" );
    for (i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
    {
        int mib[4] = { 1, 14, forms[i].number, pid }, size = 0;
        size_t len = kinfo_size;
        const long err = kstate_sysctl( mib, forms[i].mib_len, kinfo, &len );

        memcpy( &size, kinfo, sizeof(size) );
        ps4_line_str( &l, i ? ", " : " " );
        ps4_line_str( &l, forms[i].name );
        if (err && err != -ENOMEM)
        {
            ps4_line_str( &l, " error " );
            ps4_line_num( &l, -err );
            continue;
        }
        ps4_line_str( &l, " " );
        ps4_line_num( &l, (long long)len );
        ps4_line_str( &l, " bytes (records of " );
        ps4_line_num( &l, len >= sizeof(int) ? size : 0 );
        ps4_line_str( &l, ")" );
    }
    log_line( &l );
}

/* The app's program processes the desktop started, with their programs' file names: the freeze
 * report's list, and the desktop's line as one ends (written under port/ps4_main.c's children lock,
 * read without it). */
static struct { int32_t pid; char name[32]; } kids[MAX_PIDS];

static void kids_update( int pid, const char *image )
{
    const char *base = image, *p;
    int i;

    for (i = 0; i < MAX_PIDS; i++)
    {
        if (image && !kids[i].pid)
        {
            for (p = image; *p; p++)
                if (*p == '\\' || *p == '/') base = p + 1;
            memset( kids[i].name, 0, sizeof(kids[i].name) );
            strncpy( kids[i].name, base, sizeof(kids[i].name) - 1 );
            __atomic_store_n( &kids[i].pid, pid, __ATOMIC_RELEASE );
            return;
        }
        if (!image && kids[i].pid == pid)
        {
            __atomic_store_n( &kids[i].pid, 0, __ATOMIC_RELEASE );
            return;
        }
    }
}

/* the file name of the program process pid runs: 0 if it isn't one the desktop started */
int ps4_child_name( int pid, char *name, size_t size )
{
    int i;

    for (i = 0; i < MAX_PIDS; i++)
        if (__atomic_load_n( &kids[i].pid, __ATOMIC_ACQUIRE ) == pid)
        {
            snprintf( name, size, "%s", kids[i].name );
            return 1;
        }
    return 0;
}

static int record_int( const unsigned char *e, int offset )
{
    int value;

    memcpy( &value, e + offset, sizeof(value) );
    return value;
}

/* the 8 bytes after the thread id, as four 16-bit numbers: the thread's priority (FreeBSD 9's struct
 * priority, four bytes, is there; the PS4's records are 8 bytes longer and its priorities need more than
 * a byte), told as found ("pri a/b/c/d") */
static void put_priority( struct ps4_line *l, const unsigned char *e )
{
    int i;

    ps4_line_str( l, " pri " );
    for (i = 0; i < 4 && kl.tid + 4 + 2 * i + 2 <= kl.size; i++)
    {
        uint16_t value;

        memcpy( &value, e + kl.tid + 4 + 2 * i, sizeof(value) );
        if (i) ps4_line_str( l, "/" );
        ps4_line_num( l, value );
    }
}

/* This kernel's layout of kern.proc's record, from the calling thread's own record in kern.proc.all
 * (a record a thread of every process): its size, and where the pid, the thread id and the thread's
 * name are; the fields next to the name (the state, the wait's message, the lock's name) are taken to
 * move with it, the state checked against this thread's own (running). Told in wine.log. */
static void calibrate( const char *own_name )
{
    int mib[3] = { 1 /* CTL_KERN */, 14 /* KERN_PROC */, 0 /* KERN_PROC_ALL */ };
    const int pid = getpid(), tid = pthread_getthreadid_np(), name_len = (int)strlen( own_name );
    size_t len = kinfo_size;
    struct ps4_line l;
    long err;
    int size = 0, n, i;

    if (!kinfo) return;
    err = kstate_sysctl( mib, 3, kinfo, &len );
    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: kern.proc.all " );
    if (err && err != -ENOMEM)
    {
        ps4_line_str( &l, "error " );
        ps4_line_num( &l, -err );
        ps4_line_str( &l, ": no kernel's view in a freeze report" );
        log_line( &l );
        return;
    }
    if (len >= sizeof(size)) memcpy( &size, kinfo, sizeof(size) );
    ps4_line_num( &l, (long long)(size > 0 ? len / (size_t)size : 0) );
    ps4_line_str( &l, " records of " );
    ps4_line_num( &l, size );
    ps4_line_str( &l, " bytes" );
    if (size < 1024 || size > 4096)
    {
        ps4_line_str( &l, ": not a kinfo_proc, no kernel's view in a freeze report" );
        kl.size = 0;
        log_line( &l );
        return;
    }
    kl.size = size;
    n = (int)(len / (size_t)size);
    for (i = 0; i < n; i++)
    {
        const unsigned char *e = kinfo + (size_t)i * size;
        int name_at = -1, tid_at = -1, pid_at = -1, off, shift;

        if (394 + name_len < size && !memcmp( e + 394, own_name, name_len + 1 )) name_at = 394;
        for (off = 300; name_at < 0 && off + name_len < size && off < 800; off++)
            if (!memcmp( e + off, own_name, name_len + 1 )) name_at = off;
        if (name_at < 0) continue;
        shift = name_at - 394;
        if (record_int( e, 72 ) == pid) pid_at = 72;
        for (off = 40; pid_at < 0 && off < 160; off += 4)
            if (record_int( e, off ) == pid) pid_at = off;
        for (off = 520; off + 4 <= size; off += 4)
            if (record_int( e, off ) == tid && (tid_at < 0 || abs( off - 600 - shift ) < abs( tid_at - 600 - shift )))
                tid_at = off;
        if (pid_at < 0 || tid_at < 0) continue;
        kl.pid = pid_at;
        kl.wchan = pid_at - 8;
        kl.tid = tid_at;
        kl.tdname = name_at;
        kl.wmesg = 411 + shift;
        kl.lockname = 438 + shift;
        kl.stat = 388 + shift;
        kl.oncpu = 392 + shift;
        kl.kiflag = 376 + shift;
        kl.slptime = 316 + shift;
        kl.runtime = 328 + shift;
        ps4_line_str( &l, "; this thread's: pid at " );
        ps4_line_num( &l, pid_at );
        ps4_line_str( &l, ", thread id at " );
        ps4_line_num( &l, tid_at );
        ps4_line_str( &l, ", name at " );
        ps4_line_num( &l, name_at );
        ps4_line_str( &l, " (FreeBSD 9: 72, 600, 394), state " );
        ps4_line_num( &l, e[kl.stat] );
        ps4_line_str( &l, e[kl.stat] == 2 ? " (running: the layout holds)" : " (not running: states may be misread)" );
        ps4_line_str( &l, "; its priority " );
        ps4_line_num( &l, ps4_kit_thread_priority() );
        ps4_line_str( &l, " is in its record as" );
        put_priority( &l, e );
        log_line( &l );
        return;
    }
    ps4_line_str( &l, "; this thread's own not found: FreeBSD 9's offsets" );
    log_line( &l );
}

/* Each thread of process pid among the n records read: a line into fd (hang.log), and how many wait
 * for what into wine.log. */
static void report_process( long fd, int n, int pid, const char *name )
{
    struct { char key[16]; int count; } kinds[12];
    struct ps4_line l, sum;
    int i, j, nkinds = 0, threads = 0;

    sum.len = 0;
    ps4_line_str( &sum, "wineps4: kernel: " );
    ps4_line_str( &sum, name );
    ps4_line_str( &sum, " (pid " );
    ps4_line_num( &sum, pid );
    for (i = 0; i < n; i++)
    {
        const unsigned char *e = kinfo + (size_t)i * kl.size;
        const int stat = e[kl.stat];
        int tid, slept;
        long kiflag;
        uint64_t wchan, runtime;
        char key[16];
        struct ps4_line k;

        if (record_int( e, kl.pid ) != pid) continue;
        threads++;
        tid = record_int( e, kl.tid );
        slept = record_int( e, kl.slptime );
        memcpy( &kiflag, e + kl.kiflag, sizeof(kiflag) );
        memcpy( &wchan, e + kl.wchan, sizeof(wchan) );
        memcpy( &runtime, e + kl.runtime, sizeof(runtime) );

        l.len = 0;
        ps4_line_str( &l, "-- " );
        ps4_line_num( &l, pid );
        ps4_line_str( &l, " " );
        ps4_line_str( &l, name );
        ps4_line_str( &l, ", thread " );
        ps4_line_num( &l, tid );
        if (e[kl.tdname])
        {
            ps4_line_str( &l, " '" );
            put_name( &l, e + kl.tdname, 16 );
            ps4_line_str( &l, "'" );
        }
        put_priority( &l, e );
        k.len = 0;
        if (stat == 7 /* SLOCK */ || (kiflag & KI_LOCKBLOCK))
        {
            ps4_line_str( &l, ": blocked on the kernel lock '" );
            put_name( &l, e + kl.lockname, 8 );
            ps4_line_str( &l, "'" );
            ps4_line_str( &k, "lock " );
            put_name( &k, e + kl.lockname, 8 );
        }
        else if (stat == 3 /* SSLEEP */)
        {
            ps4_line_str( &l, ": asleep in " );
            put_name( &l, e + kl.wmesg, 8 );
            ps4_line_str( &l, " for " );
            ps4_line_num( &l, slept );
            ps4_line_str( &l, " s" );
            put_name( &k, e + kl.wmesg, 8 );
        }
        else if (stat == 2 /* SRUN */)
        {
            ps4_line_str( &l, ": running" );
            if (e[kl.oncpu] != 0xff)
            {
                ps4_line_str( &l, " on core " );
                ps4_line_num( &l, e[kl.oncpu] );
            }
            ps4_line_str( &k, "running" );
        }
        else
        {
            ps4_line_str( &l, stat == 4 ? ": stopped" : stat == 5 ? ": ended" : stat == 6 ? ": waiting" : ": state " );
            if (stat < 4 || stat > 6) ps4_line_num( &l, stat );
            ps4_line_str( &k, stat == 4 ? "stopped" : stat == 5 ? "ended" : stat == 6 ? "waiting" : "other" );
        }
        if (wchan && stat != 2)
        {
            ps4_line_str( &l, " (at " );
            ps4_line_hex( &l, wchan );
            ps4_line_str( &l, ")" );
        }
        ps4_line_str( &l, ", ran " );
        ps4_line_seconds( &l, runtime / 1e6, 1 );
        ps4_line_str( &l, " s\n" );
        put_text( fd, &l );

        k.text[k.len] = 0;
        memcpy( key, k.text, sizeof(key) - 1 );
        key[sizeof(key) - 1] = 0;
        for (j = 0; j < nkinds && strcmp( kinds[j].key, key ); j++) ;
        if (j == nkinds && nkinds < (int)(sizeof(kinds) / sizeof(kinds[0])))
        {
            memcpy( kinds[j].key, key, sizeof(key) );
            kinds[j].count = 0;
            nkinds++;
        }
        if (j < nkinds) kinds[j].count++;
    }
    if (!threads)
    {
        ps4_line_str( &sum, "): none of its threads among the kernel's " );
        ps4_line_num( &sum, n );
        ps4_line_str( &sum, " records" );
        log_line( &sum );
        return;
    }
    ps4_line_str( &sum, ", " );
    ps4_line_num( &sum, threads );
    ps4_line_str( &sum, " threads):" );
    /* the most first */
    for (i = 0; i < nkinds; i++)
    {
        int best = i;
        for (j = i + 1; j < nkinds; j++)
            if (kinds[j].count > kinds[best].count) best = j;
        if (best != i)
        {
            const typeof(kinds[0]) swap = kinds[i];
            kinds[i] = kinds[best];
            kinds[best] = swap;
        }
        ps4_line_str( &sum, i ? ", " : " " );
        ps4_line_num( &sum, kinds[i].count );
        ps4_line_str( &sum, " " );
        ps4_line_str( &sum, kinds[i].key );
    }
    log_line( &sum );
}

/* every process of the app: the desktop, then each program the desktop started, from one list of
 * every thread of the system */
static void report_all( const char *reason )
{
    long fd = kstate_syscall3( 5 /* SYS_open */, (long)"/data/WinePS4/hang.log", 0x0001 | 0x0008 | 0x0200, 0666 );
    int mib[3] = { 1 /* CTL_KERN */, 14 /* KERN_PROC */, 0 /* KERN_PROC_ALL */ };
    size_t len = kinfo_size;
    struct ps4_line l;
    long err = 0;
    int i, n = 0, desktop = heartbeat ? heartbeat->desktop_pid : (int)getpid();

    l.len = 0;
    ps4_line_str( &l, "==== " );
    ps4_line_str( &l, reason );
    ps4_line_str( &l, ": the kernel's view of the app's threads (port/ps4_kstate.c)\n" );
    put_text( fd, &l );
    if (kinfo && kl.size)
    {
        err = kstate_sysctl( mib, 3, kinfo, &len );
        if (!err || err == -ENOMEM) n = (int)(len / (size_t)kl.size);
    }
    if (!n)
    {
        l.len = 0;
        ps4_line_str( &l, "wineps4: kernel: kern.proc.all told nothing (error " );
        ps4_line_num( &l, -err );
        ps4_line_str( &l, !kinfo ? ", no buffer for it)" : kl.size ? ")" : ", its records unread)" );
        log_line( &l );
    }
    else
    {
        report_process( fd, n, desktop, "the desktop" );
        for (i = 0; i < MAX_PIDS; i++)
        {
            const int pid = __atomic_load_n( &kids[i].pid, __ATOMIC_ACQUIRE );
            char name[32];

            if (pid <= 0 || pid == desktop) continue;
            memcpy( name, kids[i].name, sizeof(name) );
            name[sizeof(name) - 1] = 0;
            report_process( fd, n, pid, name[0] ? name : "a program" );
        }
    }
    if (fd >= 0) kstate_syscall3( 6 /* SYS_close */, fd, 0, 0 );
}

/* --- The desktop ----------------------------------------------------------------------------------- */

/* Settings > Diagnostics > Freeze report (WINEPS4_FREEZE_REPORT=0: off, the programs' watch too) */
static int report_off( void )
{
    const char *setting = getenv( "WINEPS4_FREEZE_REPORT" );
    return setting && !strcmp( setting, "0" );
}

/* The shared page, made new as the desktop starts (before any program). Not any more (v00.67): the
 * PS4 answers no form of kern.proc with threads (v00.65), so a program that sees the desktop stand
 * has nothing to tell, and the page is a file's: the desktop's loop and its freeze watch stored to it,
 * and a store to a written-back file page is a fault, which may stand with everything else (v00.66's
 * freeze watch never wrote its report). An earlier session's page goes; the programs find none. */
void ps4_heartbeat_open( void )
{
    is_desktop = 1;
    unlink( HEARTBEAT_FILE );
}

void ps4_heartbeat_beat( unsigned int beats )
{
    if (heartbeat) __atomic_store_n( &heartbeat->beats, beats, __ATOMIC_RELAXED );
}

void ps4_heartbeat_requests( unsigned int requests )
{
    if (heartbeat) __atomic_store_n( &heartbeat->requests, requests, __ATOMIC_RELAXED );
}

/* a program process started (image: its program, a Windows or Unix path) or ended (image NULL); the
 * caller serializes these */
void ps4_heartbeat_child( int pid, const char *image )
{
    const char *base = image;
    int i;

    if (pid <= 0) return;
    kids_update( pid, image );
    if (!heartbeat) return;
    for (i = 0; i < MAX_PIDS; i++)
    {
        if (image && !heartbeat->pids[i])
        {
            const char *p;
            for (p = image; *p; p++)
                if (*p == '\\' || *p == '/') base = p + 1;
            memset( heartbeat->names[i], 0, sizeof(heartbeat->names[i]) );
            strncpy( heartbeat->names[i], base, sizeof(heartbeat->names[i]) - 1 );
            __atomic_store_n( &heartbeat->pids[i], pid, __ATOMIC_RELEASE );
            return;
        }
        if (!image && heartbeat->pids[i] == pid)
        {
            __atomic_store_n( &heartbeat->pids[i], 0, __ATOMIC_RELEASE );
            return;
        }
    }
}

/* the desktop's buffer for kern.proc, its pages touched now: the freeze may be the memory's */
void ps4_kstate_prepare( void )
{
    void *buffer = NULL;

    if (!kinfo && !sceKernelMapFlexibleMemory( &buffer, KINFO_BUFFER, PROT_READ | PROT_WRITE, 0 ))
    {
        kinfo = buffer;
        kinfo_size = KINFO_BUFFER;
    }
    if (kinfo) memset( kinfo, 0, kinfo_size );
}

/* the desktop's freeze report (its own thread) */
void ps4_kstate_report( const char *reason )
{
    struct ps4_line l;

    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: the app's flexible memory " );
    ps4_line_num( &l, (long long)(flexible_free() >> 20) );
    ps4_line_str( &l, " MiB free (v00.63 to v00.65 froze as it ran out)" );
    log_line( &l );
    memory_probe_wake();  /* (its threads touch memory while the kernel is asked) */
    report_all( reason );
    memory_probe_report();
    if (heartbeat) __atomic_store_n( &heartbeat->told, __atomic_load_n( &heartbeat->beats, __ATOMIC_RELAXED ) | 1,
                                     __ATOMIC_RELEASE );
}

/* --- The programs ---------------------------------------------------------------------------------- */

/* Each second, from a program's stats thread: when the desktop's loop has stood for WATCH_SECONDS and
 * the desktop hasn't told it (its own report may be what stands), the first program to see it tells
 * where the kernel has every thread of the app. Once a stand. */
void ps4_heartbeat_check( void )
{
    static int tried, still, told;
    static uint32_t last;
    uint32_t beats, told_at;
    int32_t expected = 0;
    struct ps4_line reason;

    if (is_desktop) return;
    if (!heartbeat)
    {
        int fd;
        void *page;

        if (tried) return;
        tried = 1;
        if (report_off() || (fd = open( HEARTBEAT_FILE, O_RDWR )) < 0) return;
        page = mmap( NULL, HEARTBEAT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
        close( fd );
        if (page == MAP_FAILED) return;
        heartbeat = page;
        last = __atomic_load_n( &heartbeat->beats, __ATOMIC_RELAXED );
        return;
    }
    if (__atomic_load_n( &heartbeat->magic, __ATOMIC_ACQUIRE ) != HEARTBEAT_MAGIC) return;
    beats = __atomic_load_n( &heartbeat->beats, __ATOMIC_RELAXED );
    if (beats != last || !beats)
    {
        last = beats;
        still = 0;
        if (told)
        {
            expected = getpid();
            __atomic_compare_exchange_n( &heartbeat->watcher, &expected, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE );
            told = 0;
        }
        return;
    }
    if (++still < WATCH_SECONDS || told) return;
    told = 1;  /* (this stand: by this process, the desktop or another) */
    told_at = __atomic_load_n( &heartbeat->told, __ATOMIC_ACQUIRE );
    if (told_at == (beats | 1)) return;  /* (the desktop's own report got through) */
    if (!__atomic_compare_exchange_n( &heartbeat->watcher, &expected, (int32_t)getpid(), 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE ))
        return;
    reason.len = 0;
    ps4_line_str( &reason, "process " );
    ps4_line_num( &reason, getpid() );
    ps4_line_str( &reason, ": the desktop's loop has stood for " );
    ps4_line_num( &reason, still );
    ps4_line_str( &reason, " s and the desktop told nothing" );
    reason.text[reason.len] = 0;
    log_line( &reason );
    report_all( reason.text );
}

/* a thread's name, for the kernel's view */
void ps4_thread_name( const char *name )
{
    pthread_set_name_np( pthread_self(), name );
    ps4_thread_table_name( name );  /* (the signals' dump's names) */
}

/* --- What a process takes of the app's flexible memory ---------------------------------------------- */

/* sceKernelVirtualQuery's record (72 bytes: shadPS4's OrbisVirtualQueryInfo) */
struct vq_info
{
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint8_t kind;  /* 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed */
    char name[32];
    uint8_t pad[7];
};
int sceKernelVirtualQuery( const void *addr, int flags, struct vq_info *info, size_t size );

/* This process's mappings (v00.65: the app's flexible memory ran out as a game started, Steam's web
 * helper holding 465 MiB of it): flexible ones (every anonymous mapping, the kernel's, accessible or
 * not, charged whole) and shared ones that are neither flexible nor direct (shared memory, files:
 * Wine's sections, charged as touched), in bytes. 0: the kernel told nothing. */
int ps4_memory_census( uint64_t *flexible, uint64_t *shared )
{
    struct vq_info info;
    const char *addr = NULL;
    int n = 0;

    *flexible = *shared = 0;
    while (n < 100000 && !sceKernelVirtualQuery( addr, 1 /* the next mapping from addr */, &info, sizeof(info) ))
    {
        const uint64_t size = info.end > info.start ? info.end - info.start : 0;

        if (info.kind & 1) *flexible += size;
        else if (!(info.kind & 2) && info.protection) *shared += size;
        if (info.end <= (uint64_t)(uintptr_t)addr) break;
        addr = (const char *)(uintptr_t)info.end;
        n++;
    }
    return n > 0;
}

/* the app's flexible memory free: the pool every local process takes from */
static uint64_t flexible_free( void )
{
    size_t size = 0;

    return sceKernelAvailableFlexibleMemorySize( &size ) ? 0 : size;
}

/* A program's flexible mappings by name, kind and memory type, the largest first, into wine.log (its
 * standard error): what the census counts, told when it is large (v00.66: Steam's web helper had 390
 * to 456 MiB flexible and nothing "shared", which the memory layer can't account for). */
void ps4_memory_breakdown( const char *who )
{
    struct { char name[32]; int32_t type, prot; uint8_t kind; uint64_t size; unsigned int count; } top[40];
    struct vq_info info;
    const char *addr = NULL;
    char line[1200];
    int n = 0, len, i, j, steps = 0;

    while (steps++ < 100000 && !sceKernelVirtualQuery( addr, 1, &info, sizeof(info) ))
    {
        const uint64_t size = info.end > info.start ? info.end - info.start : 0;

        if (info.end <= (uint64_t)(uintptr_t)addr) break;
        addr = (const char *)(uintptr_t)info.end;
        if (!(info.kind & 1)) continue;
        info.name[sizeof(info.name) - 1] = 0;
        for (i = 0; i < n; i++)
            if (top[i].type == info.memory_type && top[i].prot == info.protection && top[i].kind == info.kind &&
                !strcmp( top[i].name, info.name ))
                break;
        if (i == n)
        {
            if (n == (int)(sizeof(top) / sizeof(top[0]))) continue;
            memcpy( top[n].name, info.name, sizeof(top[n].name) );
            top[n].type = info.memory_type;
            top[n].prot = info.protection;
            top[n].kind = info.kind;
            top[n].size = 0;
            top[n].count = 0;
            n++;
        }
        top[i].size += size;
        top[i].count++;
    }
    len = snprintf( line, sizeof(line), "wineps4: memory: %s's flexible mappings, the largest first:", who );
    for (i = 0; i < 10 && i < n; i++)
    {
        int best = i;
        for (j = i + 1; j < n; j++)
            if (top[j].size > top[best].size) best = j;
        if (best != i)
        {
            const typeof(top[0]) swap = top[i];
            top[i] = top[best];
            top[best] = swap;
        }
        len += snprintf( line + len, sizeof(line) - len, "%s '%s' %llu MiB in %u (type %d, prot %#x, kind %#x)",
                         i ? "," : "", top[i].name, (unsigned long long)(top[i].size >> 20), top[i].count,
                         top[i].type, top[i].prot, top[i].kind );
        if (len >= (int)sizeof(line)) break;
    }
    fprintf( stderr, "%s\n", line );
}

/* --- What the desktop's threads are in, without a signal --------------------------------------------- */

/* This thread's stack, size bytes below here, touched now: a page first touched at a freeze is a
 * fault, and a fault can stand with everything else (v00.66's freeze watch never wrote its report). */
void ps4_touch_stack( size_t size )
{
    volatile char *p = __builtin_alloca( size );
    size_t i;

    for (i = 0; i < size; i += 4096) p[i] = 0;
}

/* As the desktop starts, on the kernel report's thread: which forms of kern.proc this kernel answers,
 * its record's layout (port/ps4_main.c names this thread "kernel report"), and the memory probe's
 * threads */
void ps4_kstate_probe( void )
{
    probe_kern_proc( getpid() );
    calibrate( "kernel report" );
    memory_probe_start();
}

static uint64_t now_us( void )
{
    struct timespec t;

    clock_gettime( CLOCK_MONOTONIC, &t );
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

/* The memory calls each thread is in (the memory layer's, wineserver's shared memory): a thread that
 * stands in one is seen in the freeze report by this table, as no signal reaches it (v00.64 to v00.66).
 * A place a thread, taken at its first call; not nested. */
#define MAX_CALL_SLOTS 512
static struct { int tid; const char *what; uint64_t since; } call_slots[MAX_CALL_SLOTS];
static __thread int call_slot = -1;

void ps4_call_enter( const char *what )
{
    if (call_slot < 0)
    {
        const int tid = pthread_getthreadid_np();
        int i;

        for (i = 0; i < MAX_CALL_SLOTS && call_slot < 0; i++)
        {
            int expected = 0;
            if (__atomic_compare_exchange_n( &call_slots[i].tid, &expected, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
                call_slot = i;
        }
        if (call_slot < 0) return;
    }
    call_slots[call_slot].since = now_us();
    __atomic_store_n( &call_slots[call_slot].what, what, __ATOMIC_RELEASE );
}

void ps4_call_leave( void )
{
    if (call_slot >= 0) __atomic_store_n( &call_slots[call_slot].what, NULL, __ATOMIC_RELEASE );
}

/* the desktop's loop: what it is doing, since when (shell/desktop.cpp) */
static const char *volatile desktop_phase = "starting";
static volatile uint64_t desktop_phase_since;

void ps4_desktop_phase( const char *phase )
{
    desktop_phase_since = now_us();
    desktop_phase = phase;
}

/* For the freeze report: where the desktop's loop is, and the threads in a memory call for a second
 * or more ("; the desktop's loop presenting for 7 s; in memory calls: thread 101 in mmap for 7 s") */
void ps4_calls_report( struct ps4_line *l )
{
    const uint64_t now = now_us();
    int i, n = 0;

    ps4_line_str( l, "; the desktop's loop " );
    ps4_line_str( l, desktop_phase );
    ps4_line_str( l, " for " );
    ps4_line_num( l, (long long)((now - desktop_phase_since) / 1000000) );
    ps4_line_str( l, " s; in memory calls:" );
    for (i = 0; i < MAX_CALL_SLOTS; i++)
    {
        const char *what = __atomic_load_n( &call_slots[i].what, __ATOMIC_ACQUIRE );
        const uint64_t since = call_slots[i].since;

        if (!what || now < since + 1000000) continue;
        ps4_line_str( l, n++ ? ", thread " : " thread " );
        ps4_line_num( l, call_slots[i].tid );
        ps4_line_str( l, " in " );
        ps4_line_str( l, what );
        ps4_line_str( l, " for " );
        ps4_line_num( l, (long long)((now - since) / 1000000) );
        ps4_line_str( l, " s" );
    }
    if (!n) ps4_line_str( l, " none" );
}

/* --- The memory probe ------------------------------------------------------------------------------ */

/* At a freeze, a thread for each kind of memory the desktop has touches it, and the report tells which
 * answered and which stood: v00.64 to v00.69's stuck threads were the desktop's loop copying a frame
 * into the screen, wineserver in its heap and, in v00.69, the freeze watch in a query of the app's
 * direct memory, while the threads that waited in system calls answered. A probe still standing at
 * the next freeze is told so, not asked again. */
#define PROBE_STEPS 5
#define PROBE_PAGE  16384

int32_t sceKernelMunmap( void *addr, size_t len );
int32_t sceKernelAllocateDirectMemory( off_t start, off_t end, size_t len, size_t align, int type, off_t *phys );
int32_t sceKernelMapDirectMemory( void **addr, size_t len, int prot, int flags, off_t phys, size_t align );
int32_t sceKernelReleaseDirectMemory( off_t start, size_t len );
int32_t sceKernelAvailableDirectMemorySize( off_t start, off_t end, size_t align, off_t *phys, size_t *size );
size_t sceKernelGetDirectMemorySize( void );
volatile uint32_t *ps4_screen_probe_pixel( void );  /* (shell/screen.cpp) */

static const char *const probe_names[PROBE_STEPS] =
{
    "an old page of the heap", "the screen", "the direct memory query", "a new page of flexible memory",
    "a new page of direct memory",
};
static struct
{
    int alive;           /* its thread runs */
    volatile int state;  /* 0 idle, 1 asked, 2 answered */
    unsigned int ms;
    int error;           /* what the kernel refused it with, 0 none */
    int stale;           /* still asked from an earlier freeze */
} probes[PROBE_STEPS];
/* (no descriptors: the app has ~250 for all its processes; the lock is held only by probes waiting to
 * be asked, never by one in its step) */
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_asked_cond = PTHREAD_COND_INITIALIZER;
static volatile unsigned char *probe_heap;  /* 64 KiB of the desktop's heap, written as it starts */
static uint64_t probe_asked;

static int probe_run( int step )
{
    void *p = NULL;
    off_t phys = 0;
    size_t largest = 0;
    int ret = 0;

    switch (step)
    {
    case 0:  /* a page of the desktop's heap (direct memory), written as the desktop started */
        if (probe_heap) probe_heap[PROBE_PAGE] = (unsigned char)(probe_heap[PROBE_PAGE] + 1);
        break;
    case 1:  /* the screen: the buffers the display reads (write-combined direct memory) */
    {
        volatile uint32_t *pixel = ps4_screen_probe_pixel();
        if (pixel) (void)*pixel;
        break;
    }
    case 2:  /* how much of the app's direct memory is free */
        ret = sceKernelAvailableDirectMemorySize( 0, (off_t)sceKernelGetDirectMemorySize(), PROBE_PAGE, &phys, &largest );
        break;
    case 3:  /* a new page of flexible memory */
        if (!(ret = sceKernelMapFlexibleMemory( &p, PROBE_PAGE, PROT_READ | PROT_WRITE, 0 )))
        {
            *(volatile char *)p = 1;
            sceKernelMunmap( p, PROBE_PAGE );
        }
        break;
    case 4:  /* a new page of direct memory */
        if (!(ret = sceKernelAllocateDirectMemory( 0, (off_t)sceKernelGetDirectMemorySize(), PROBE_PAGE, PROBE_PAGE, 0,
                                                   &phys )))
        {
            if (!(ret = sceKernelMapDirectMemory( &p, PROBE_PAGE, PROT_READ | PROT_WRITE, 0, phys, PROBE_PAGE )))
            {
                *(volatile char *)p = 1;
                sceKernelMunmap( p, PROBE_PAGE );
            }
            sceKernelReleaseDirectMemory( phys, PROBE_PAGE );
        }
        break;
    }
    return ret;
}

static void *probe_thread( void *arg )
{
    const int step = (int)(long)arg;
    sigset_t set;

    sigfillset( &set );
    pthread_sigmask( SIG_BLOCK, &set, NULL );
    ps4_kit_set_thread_priority( PS4_PRIO_REPORT );
    ps4_thread_name( "memory probe" );
    ps4_touch_stack( 16 << 10 );
    for (;;)
    {
        uint64_t start;

        pthread_mutex_lock( &probe_lock );
        while (__atomic_load_n( &probes[step].state, __ATOMIC_ACQUIRE ) != 1)
            pthread_cond_wait( &probe_asked_cond, &probe_lock );
        pthread_mutex_unlock( &probe_lock );
        start = now_us();
        probes[step].error = probe_run( step );
        probes[step].ms = (unsigned int)((now_us() - start) / 1000);
        __atomic_store_n( &probes[step].state, 2, __ATOMIC_RELEASE );
    }
    return NULL;
}

/* the desktop, as it starts: the heap's block, and a thread for each step */
static void memory_probe_start( void )
{
    struct vq_info info;
    struct ps4_line l;
    pthread_t thread;
    int i, started = 0;

    if ((probe_heap = malloc( 4 * PROBE_PAGE ))) memset( (void *)probe_heap, 0, 4 * PROBE_PAGE );
    for (i = 0; i < PROBE_STEPS; i++)
    {
        if (pthread_create( &thread, NULL, probe_thread, (void *)(long)i )) continue;
        pthread_detach( thread );
        probes[i].alive = 1;
        started++;
    }
    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: memory probe: " );
    ps4_line_num( &l, started );
    ps4_line_str( &l, " threads; its block of the heap is " );
    memset( &info, 0, sizeof(info) );
    if (!probe_heap) ps4_line_str( &l, "missing" );
    else if (sceKernelVirtualQuery( (const void *)probe_heap, 0, &info, sizeof(info) )) ps4_line_str( &l, "unknown to the kernel" );
    else ps4_line_str( &l, info.kind & 2 ? "direct memory" : info.kind & 1 ? "flexible memory" : "other memory" );
    log_line( &l );
}

static void memory_probe_wake( void )
{
    int i;

    probe_asked = now_us();
    pthread_mutex_lock( &probe_lock );
    for (i = 0; i < PROBE_STEPS; i++)
    {
        if (!probes[i].alive) continue;
        if ((probes[i].stale = __atomic_load_n( &probes[i].state, __ATOMIC_ACQUIRE ) == 1)) continue;
        __atomic_store_n( &probes[i].state, 1, __ATOMIC_RELEASE );
    }
    pthread_cond_broadcast( &probe_asked_cond );
    pthread_mutex_unlock( &probe_lock );
}

/* waits until 2 s after they were asked, then tells each */
static void memory_probe_report( void )
{
    const struct { long sec, nsec; } tick = { 0, 50000000 };
    struct ps4_line l;
    int i, asked;

    for (;;)
    {
        for (i = asked = 0; i < PROBE_STEPS; i++)
            if (!probes[i].stale && __atomic_load_n( &probes[i].state, __ATOMIC_ACQUIRE ) == 1) asked++;
        if (!asked || now_us() > probe_asked + 2000000) break;
        kstate_syscall3( 240 /* SYS_nanosleep */, (long)&tick, 0, 0 );
    }
    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: memory probe:" );
    for (i = 0; i < PROBE_STEPS; i++)
    {
        const int state = __atomic_load_n( &probes[i].state, __ATOMIC_ACQUIRE );

        if (!probes[i].alive) continue;
        ps4_line_str( &l, i ? ", " : " " );
        ps4_line_str( &l, probe_names[i] );
        if (state == 1)
        {
            ps4_line_str( &l, probes[i].stale ? " still standing from an earlier freeze" : " STOOD (2 s and more)" );
            continue;
        }
        if (probes[i].error)
        {
            ps4_line_str( &l, " refused (" );
            ps4_line_hex( &l, (unsigned int)probes[i].error );
            ps4_line_str( &l, ")" );
        }
        else
        {
            ps4_line_str( &l, " answered in " );
            ps4_line_num( &l, probes[i].ms );
            ps4_line_str( &l, " ms" );
        }
        __atomic_store_n( &probes[i].state, 0, __ATOMIC_RELEASE );
    }
    log_line( &l );
}
