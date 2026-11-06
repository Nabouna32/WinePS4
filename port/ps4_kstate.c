/* WinePS4: where the kernel has the app's threads, read without a signal: the freeze report's other
 * half. v00.64's thread dump had two of the desktop's threads that never answered its signal, then
 * stood itself on the next one: they waited in the kernel, where a signal waits until they return.
 * __sysctl kern.proc.pid with threads tells each thread's state and what it waits on: its sleep's
 * message (umtxn a mutex, select, accept, ufs a file's lock, pfault memory, biord the disk, ...) or the
 * kernel lock it is blocked on, and for how long. Read as FreeBSD 9's kinfo_proc for amd64 (1088
 * bytes); another size is only told.
 *
 * The desktop asks at a freeze (port/ps4_main.c, on a thread of its own: the report's signals and
 * this don't wait for each other). Asking wires the asker's buffer, which takes its address space's
 * lock, so a desktop whose address space is what stands can't ask. The program processes watch it
 * too, each second from their stats thread (port/ps4_spawn_desk.c): the desktop's loop counts in a
 * page of shared memory (/data/WinePS4/.heartbeat) with the app's processes and their names, and the
 * first program to see it still for 10 s, when the desktop hasn't told it already, asks for every
 * process of the app. All of it without printf, malloc or a lock a stuck thread may hold. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "include/ps4_line.h"

/* FreeBSD 9's struct kinfo_proc for amd64 (sys/user.h): what is read of it */
#define KINFO_SIZE     1088
#define KI_WCHAN       64
#define KI_PID         72
#define KI_SLPTIME     316   /* seconds since it went to sleep */
#define KI_RUNTIME     328   /* microseconds it ran */
#define KI_KIFLAG      376
#define KI_STAT        388
#define KI_ONCPU       392
#define KI_TDNAME      394   /* 17 bytes */
#define KI_WMESG       411   /* 9 */
#define KI_LOCKNAME    438   /* 9 */
#define KI_TID         600
#define KI_LOCKBLOCK   0x4   /* (ki_kiflag) blocked on a kernel lock: ki_lockname says which */
#define MAX_KTHREADS   256

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

static unsigned char kinfo[MAX_KTHREADS * KINFO_SIZE] __attribute__((aligned(16)));
static struct heartbeat *heartbeat;
static int is_desktop;

int sceKernelAvailableFlexibleMemorySize( size_t *size );
int pthread_getthreadid_np( void );
static uint64_t flexible_free( void );

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

    if (done) return;
    done = 1;
    l.len = 0;
    ps4_line_str( &l, "wineps4: kernel: kern.proc's forms here:" );
    for (i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
    {
        int mib[4] = { 1, 14, forms[i].number, pid }, size = 0;
        size_t len = sizeof(kinfo);
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

/* Each thread of process pid: a line into fd (hang.log), and how many wait for what into wine.log. */
static void report_process( long fd, int pid, const char *name )
{
    int mib[4] = { 1 /* CTL_KERN */, 14 /* KERN_PROC */, 1 | 0x10 /* KERN_PROC_PID | KERN_PROC_INC_THREAD */, pid };
    struct { char key[16]; int count; } kinds[12];
    struct ps4_line l, sum;
    size_t len = sizeof(kinfo);
    long err = kstate_sysctl( mib, 4, kinfo, &len );
    int n, i, j, nkinds = 0, size;

    sum.len = 0;
    ps4_line_str( &sum, "wineps4: kernel: " );
    ps4_line_str( &sum, name );
    ps4_line_str( &sum, " (pid " );
    ps4_line_num( &sum, pid );
    if (err && err != -ENOMEM)
    {
        ps4_line_str( &sum, "): the kernel told nothing (error " );
        ps4_line_num( &sum, -err );
        ps4_line_str( &sum, ")" );
        log_line( &sum );
        probe_kern_proc( pid );
        return;
    }
    memcpy( &size, kinfo, sizeof(size) );
    if (len < sizeof(int) || size != KINFO_SIZE)
    {
        ps4_line_str( &sum, "): its kinfo_proc has " );
        ps4_line_num( &sum, len < sizeof(int) ? 0 : size );
        ps4_line_str( &sum, " bytes, not FreeBSD 9's 1088: not read" );
        log_line( &sum );
        return;
    }
    n = (int)(len / KINFO_SIZE);
    for (i = 0; i < n; i++)
    {
        const unsigned char *e = kinfo + (size_t)i * KINFO_SIZE;
        const int stat = e[KI_STAT];
        int tid, slept, p;
        long kiflag;
        uint64_t wchan, runtime;
        char key[16];
        struct ps4_line k;

        memcpy( &tid, e + KI_TID, sizeof(tid) );
        memcpy( &slept, e + KI_SLPTIME, sizeof(slept) );
        memcpy( &kiflag, e + KI_KIFLAG, sizeof(kiflag) );
        memcpy( &wchan, e + KI_WCHAN, sizeof(wchan) );
        memcpy( &runtime, e + KI_RUNTIME, sizeof(runtime) );
        memcpy( &p, e + KI_PID, sizeof(p) );

        l.len = 0;
        ps4_line_str( &l, "-- " );
        ps4_line_num( &l, pid );
        ps4_line_str( &l, " " );
        ps4_line_str( &l, name );
        ps4_line_str( &l, ", thread " );
        ps4_line_num( &l, tid );
        if (e[KI_TDNAME])
        {
            ps4_line_str( &l, " '" );
            put_name( &l, e + KI_TDNAME, 16 );
            ps4_line_str( &l, "'" );
        }
        k.len = 0;
        if (stat == 7 /* SLOCK */ || (kiflag & KI_LOCKBLOCK))
        {
            ps4_line_str( &l, ": blocked on the kernel lock '" );
            put_name( &l, e + KI_LOCKNAME, 8 );
            ps4_line_str( &l, "'" );
            ps4_line_str( &k, "lock " );
            put_name( &k, e + KI_LOCKNAME, 8 );
        }
        else if (stat == 3 /* SSLEEP */)
        {
            ps4_line_str( &l, ": asleep in " );
            put_name( &l, e + KI_WMESG, 8 );
            ps4_line_str( &l, " for " );
            ps4_line_num( &l, slept );
            ps4_line_str( &l, " s" );
            put_name( &k, e + KI_WMESG, 8 );
        }
        else if (stat == 2 /* SRUN */)
        {
            ps4_line_str( &l, ": running" );
            if (e[KI_ONCPU] != 0xff)
            {
                ps4_line_str( &l, " on core " );
                ps4_line_num( &l, e[KI_ONCPU] );
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
        ps4_line_str( &l, " s" );
        if (p != pid) ps4_line_str( &l, " (another process's record?)" );
        ps4_line_str( &l, "\n" );
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
    ps4_line_str( &sum, ", " );
    ps4_line_num( &sum, n );
    ps4_line_str( &sum, n >= MAX_KTHREADS ? "+ threads):" : " threads):" );
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

/* every process of the app: the desktop, then each program the desktop started */
static void report_all( const char *reason )
{
    long fd = kstate_syscall3( 5 /* SYS_open */, (long)"/data/WinePS4/hang.log", 0x0001 | 0x0008 | 0x0200, 0666 );
    struct ps4_line l;
    int i, desktop = heartbeat ? heartbeat->desktop_pid : (int)getpid();

    l.len = 0;
    ps4_line_str( &l, "==== " );
    ps4_line_str( &l, reason );
    ps4_line_str( &l, ": the kernel's view of the app's threads (port/ps4_kstate.c)\n" );
    put_text( fd, &l );
    report_process( fd, desktop, "the desktop" );
    for (i = 0; heartbeat && i < MAX_PIDS; i++)
    {
        const int pid = __atomic_load_n( &heartbeat->pids[i], __ATOMIC_ACQUIRE );
        char name[32];

        if (pid <= 0 || pid == desktop) continue;
        memcpy( name, heartbeat->names[i], sizeof(name) );
        name[sizeof(name) - 1] = 0;
        report_process( fd, pid, name[0] ? name : "a program" );
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

    if (!heartbeat || pid <= 0) return;
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

/* its pages touched now: the freeze may be the memory's */
void ps4_kstate_prepare( void )
{
    memset( kinfo, 0, sizeof(kinfo) );
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
    report_all( reason );
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

/* which forms of kern.proc this kernel answers, told at the start (the report's own, at a freeze, may
 * never run) */
void ps4_kstate_probe( void )
{
    probe_kern_proc( getpid() );
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
