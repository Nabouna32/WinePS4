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
#include <stdlib.h>
#include <string.h>
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

/* the shared page, made new as the desktop starts (before any program) */
void ps4_heartbeat_open( void )
{
    int fd;
    void *page;

    is_desktop = 1;
    if (report_off())
    {
        unlink( HEARTBEAT_FILE );  /* (an earlier session's, which stands still) */
        return;
    }
    if ((fd = open( HEARTBEAT_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666 )) < 0) return;
    if (!ftruncate( fd, HEARTBEAT_SIZE ) &&
        (page = mmap( NULL, HEARTBEAT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) != MAP_FAILED)
    {
        heartbeat = page;
        memset( heartbeat, 0, sizeof(*heartbeat) );
        heartbeat->desktop_pid = getpid();
        __atomic_store_n( &heartbeat->magic, HEARTBEAT_MAGIC, __ATOMIC_RELEASE );
    }
    close( fd );
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
