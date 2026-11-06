/* WinePS4: Wine's memory on the PS4 (ntdll's and wineserver's mmap, munmap, mprotect, madvise).
 *
 * Wine reserves most of the address space with inaccessible mappings and commits pages later with
 * mprotect. On the PS4 every anonymous mapping takes flexible memory, accessible or not, and the app
 * has ~180 MiB of it left, so Wine's first reservation used it all up. Here:
 *
 *  - an anonymous PROT_NONE mapping only reserves address space (sceKernelReserveVirtualRange)
 *  - a page is backed when it first becomes accessible: by direct memory (the 4.5 GiB pool), mapped
 *    at the page and zeroed before any access is allowed, so no other thread can see it half done
 *  - if direct memory cannot run code on this console, pages are moved to system flexible memory
 *    when they get PROT_EXEC (the probe ran code from there)
 *  - file mappings at fixed addresses are refused with ENODEV, which makes Wine read the file into
 *    memory instead: code can't run from file mappings on the PS4 (the probe faulted there), and
 *    that read is what PE images need. Shared mappings (MAP_SHARED, without code) stay real file
 *    mappings: the user shared data page, writable sections, and the wineserver's session memory,
 *    which programs map read-only and must see change (a copy made them miss every window class,
 *    v00.27); so do mappings at an address of the kernel's choosing.
 *
 * Pages are tracked per 16 KiB host page in a two-level table: 0 = not ours (passed to the kernel),
 * reserved, backed by flexible memory, or the index of the direct memory page behind it.
 *
 * A process the desktop started outside the PS4's 7 (ps4_outside_process, port/ps4_main.c) backs
 * every page with regular flexible memory instead: the app's direct memory mapped in a process the
 * system doesn't know as the app's brought the PS4 down when it suspended the app (v00.42). Nothing
 * limits that memory there but the console's: WINEPS4_OUTSIDE_MAX_MB does.
 *
 *  WINEPS4_VM_LOG=1               log every operation (env.txt), off by default
 *  WINEPS4_DIRECT_RESERVE_MB=n    direct memory left to the rest of the app (default 256)
 *  WINEPS4_OUTSIDE_MAX_MB=n       the most a process outside the 7 backs (default 256) */
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int32_t sceKernelReserveVirtualRange( void **addr, size_t len, int32_t flags, size_t alignment );
int32_t sceKernelMunmap( void *addr, size_t len );
int32_t sceKernelMprotect( const void *addr, size_t len, int32_t prot );
int32_t sceKernelMapNamedSystemFlexibleMemory( void **addr, size_t len, int32_t prot, int32_t flags,
                                               const char *name );
int32_t sceKernelMapNamedFlexibleMemory( void **addr, size_t len, int32_t prot, int32_t flags, const char *name );
int32_t sceKernelAvailableFlexibleMemorySize( size_t *size );
size_t sceKernelGetDirectMemorySize( void );
int32_t sceKernelAllocateDirectMemory( off_t start, off_t end, size_t len, size_t align, int32_t type,
                                       off_t *phys );
int32_t sceKernelAvailableDirectMemorySize( off_t start, off_t end, size_t align, off_t *start_out,
                                            size_t *size_out );
int32_t sceKernelMapDirectMemory( void **addr, size_t len, int32_t prot, int32_t flags, off_t phys,
                                  size_t align );
void *__real_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset );

#define HOST_PAGE    0x4000
#define PAGE_SHIFT   14
#define SCE_FIXED    0x10
#define PROT_ALL     (PROT_READ | PROT_WRITE | PROT_EXEC)
#define USD_ADDRESS  0x7ffe0000  /* KUSER_SHARED_DATA: the server's file, kept a real mapping */

/* the table: one level-2 block of pages per GiB, 47 bits of address space */
#define L2_SHIFT     30
#define L2_PAGES     (1u << (L2_SHIFT - PAGE_SHIFT))
#define L1_ENTRIES   (1u << (47 - L2_SHIFT))
#define ADDR_LIMIT   ((uintptr_t)1 << 47)
#define GIB          ((uintptr_t)1 << L2_SHIFT)

/* where the app's own part of the address space starts: below it the PS4's (its libraries, the
 * kernel's choices of address) */
#define APP_AREA_START ((uintptr_t)0x1000000000)

enum { PAGE_UNTRACKED = 0, PAGE_RESERVED = 1, PAGE_FLEXIBLE = 2, PAGE_DIRECT = 3 /* + direct page */ };

/* direct memory, taken from the system in 64 MiB chunks */
#define CHUNK_PAGES  4096
#define MAX_CHUNKS   80
static off_t chunk_phys[MAX_CHUNKS];
static unsigned int chunk_count, chunk_used;  /* chunk_used: pages handed out of the last chunk */
static uint32_t free_pages[MAX_CHUNKS * CHUNK_PAGES];
static unsigned int free_count;
static size_t direct_reserve = 256u << 20;
static size_t memory_margin;  /* (ps4_vm_set_memory_margin) */

static uint32_t *l1[L1_ENTRIES];
static pthread_mutex_t vm_lock;
static pthread_once_t vm_once = PTHREAD_ONCE_INIT;
static int vm_log;
static int direct_exec = 1;  /* direct memory runs code (tested at start) */
static unsigned long long backed_direct, backed_flexible;  /* pages */

/* a process outside the PS4's 7: regular flexible memory for every page, flexible_max at most, and
 * none while the pool they share with the PS4 has less than the floor (v00.52: four of them took it
 * down to 13 MiB and a new local process could not start its C library: the PS4 closed the app) */
extern int ps4_outside_process;
static int flexible_only;
static unsigned long long flexible_max = (256u << 20) >> PAGE_SHIFT;  /* pages */
#define OUTSIDE_POOL_FLOOR ((size_t)128 << 20)

/* the level-2 block of a GiB whose every page is reserved, which has no table of its own yet (one is
 * 256 KiB: Chromium reserves 32 GiB at once, v00.50) */
static uint32_t reserved_block[1];
#define RESERVED_BLOCK reserved_block

#define LOG(...) do { if (vm_log) fprintf( stderr, "wineps4 vm: " __VA_ARGS__ ); } while (0)

static uint32_t page_get( uintptr_t addr )
{
    uint32_t *l2 = l1[addr >> L2_SHIFT];
    if (l2 == RESERVED_BLOCK) return PAGE_RESERVED;
    return l2 ? l2[(addr >> PAGE_SHIFT) & (L2_PAGES - 1)] : PAGE_UNTRACKED;
}

static int page_set( uintptr_t addr, uint32_t state )
{
    uint32_t **slot = &l1[addr >> L2_SHIFT];

    if (*slot == RESERVED_BLOCK)
    {
        uint32_t *table;
        unsigned int i;

        if (state == PAGE_RESERVED) return 1;
        if (!(table = malloc( L2_PAGES * sizeof(uint32_t) ))) return 0;
        for (i = 0; i < L2_PAGES; i++) table[i] = PAGE_RESERVED;
        *slot = table;
    }
    else if (!*slot)
    {
        if (state == PAGE_UNTRACKED) return 1;
        if (!(*slot = calloc( L2_PAGES, sizeof(uint32_t) ))) return 0;
    }
    (*slot)[(addr >> PAGE_SHIFT) & (L2_PAGES - 1)] = state;
    return 1;
}

/* every page of start-end reserved (a range the kernel has just reserved: nothing of ours in it);
 * whole GiBs without a table of their own */
static int reserve_pages( uintptr_t start, uintptr_t end )
{
    uintptr_t addr = start;

    while (addr < end)
    {
        const uintptr_t next = (addr | (GIB - 1)) + 1, stop = next < end ? next : end;
        uint32_t **slot = &l1[addr >> L2_SHIFT];

        if (addr == next - GIB && stop == next && (!*slot || *slot == RESERVED_BLOCK))
        {
            *slot = RESERVED_BLOCK;
            addr = stop;
            continue;
        }
        for (; addr < stop; addr += HOST_PAGE)
            if (!page_set( addr, PAGE_RESERVED )) return 0;
    }
    return 1;
}

static off_t direct_phys( uint32_t page )
{
    return chunk_phys[page / CHUNK_PAGES] + (off_t)(page % CHUNK_PAGES) * HOST_PAGE;
}

static int new_chunk( void )
{
    off_t start = 0, phys;
    size_t size = 0;

    if (chunk_count == MAX_CHUNKS) return 0;
    if (sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &size ) ||
        size < (size_t)CHUNK_PAGES * HOST_PAGE + direct_reserve)
    {
        fprintf( stderr, "wineps4 vm: direct memory used up (%zu MiB left, %zu MiB kept for the app)\n",
                 size >> 20, direct_reserve >> 20 );
        return 0;
    }
    if (sceKernelAllocateDirectMemory( 0, sceKernelGetDirectMemorySize(), (size_t)CHUNK_PAGES * HOST_PAGE,
                                       HOST_PAGE, 0, &phys ))
        return 0;
    chunk_phys[chunk_count++] = phys;
    chunk_used = 0;
    LOG( "direct memory chunk %u at %#llx\n", chunk_count - 1, (unsigned long long)phys );
    return 1;
}

/* up to max pages of direct memory with consecutive physical addresses: a run of freed pages stays
 * in its chunk, as each chunk is an allocation of its own (v00.46: a run that went on into the next
 * chunk's first pages was mapped past its chunk's end; the kernel refused it, EACCES, and Steam
 * stopped with "out of memory" while 2 GiB were free) */
static unsigned int take_direct( unsigned int max, uint32_t *first )
{
    unsigned int n = 1;

    if (free_count)
    {
        *first = free_pages[--free_count];
        while (n < max && free_count && free_pages[free_count - 1] == *first + n && (*first + n) % CHUNK_PAGES)
        {
            free_count--;
            n++;
        }
        return n;
    }
    if (!chunk_count || chunk_used == CHUNK_PAGES)
        if (!new_chunk()) return 0;
    n = CHUNK_PAGES - chunk_used;
    if (n > max) n = max;
    *first = (chunk_count - 1) * CHUNK_PAGES + chunk_used;
    chunk_used += n;
    return n;
}

/* freed in reverse, so take_direct finds them again in ascending runs */
static void give_direct( uint32_t first, unsigned int n )
{
    while (n--) free_pages[free_count++] = first + n;
}

/* what this process has backed, for Task Manager (port/ps4_spawn_desk.c; read without the lock: a
 * figure for the eye) */
unsigned long long ps4_vm_backed_bytes( void )
{
    return (backed_direct + backed_flexible) * HOST_PAGE;
}

/* Gives the pages' memory back: the range is no longer ours (the kernel's, or unmapped next). */
static void forget_range( uintptr_t start, uintptr_t end )
{
    uintptr_t addr = start;

    while (addr < end)
    {
        const uintptr_t next = (addr | (GIB - 1)) + 1, stop = next < end ? next : end;
        uint32_t **slot = &l1[addr >> L2_SHIFT];

        if (!*slot || (*slot == RESERVED_BLOCK && addr == next - GIB && stop == next))
        {
            if (*slot) *slot = NULL;  /* (a reserved GiB, whole) */
            addr = stop;
            continue;
        }
        for (; addr < stop; addr += HOST_PAGE)
        {
            uint32_t state = page_get( addr );
            if (state == PAGE_UNTRACKED) continue;
            if (state >= PAGE_DIRECT) { give_direct( state - PAGE_DIRECT, 1 ); backed_direct--; }
            else if (state == PAGE_FLEXIBLE) backed_flexible--;
            page_set( addr, PAGE_UNTRACKED );
        }
    }
}

/* Gives the pages' memory back and unmaps the range, ours or the kernel's. */
static int release_range( uintptr_t start, uintptr_t end )
{
    uintptr_t addr;
    int ret = 0;

    forget_range( start, end );
    /* (in pieces of 4 GiB at most, as reserve_high and reserve_fixed reserve them) */
    for (addr = start; addr < end; addr += (uintptr_t)4 << 30)
    {
        const uintptr_t piece = end - addr < ((uintptr_t)4 << 30) ? end - addr : (uintptr_t)4 << 30;
        if (sceKernelMunmap( (void *)addr, piece )) ret = -1;
    }
    return ret;
}

/* Backs reserved pages: zeroed while still inaccessible to everyone else, then given prot. */
static int back_range( uintptr_t start, uintptr_t end, int prot )
{
    int flexible = flexible_only || ((prot & PROT_EXEC) && !direct_exec);
    uintptr_t addr = start;

    while (addr < end)
    {
        unsigned int want = (end - addr) >> PAGE_SHIFT, n, i;
        uint32_t first = 0;
        void *base = (void *)addr;
        int ret;

        if (flexible_only)
        {
            static int told, told_pool;
            size_t pool = 0;

            n = want;
            if (backed_flexible + n > flexible_max)
            {
                if (!told++)
                    fprintf( stderr, "wineps4 vm: this process outside the PS4's 7 has backed %llu MiB, the most "
                             "it may (WINEPS4_OUTSIDE_MAX_MB)\n", backed_flexible >> 6 );
                errno = ENOMEM;
                return 0;
            }
            /* the pool it shares with the PS4 and the others outside never below the floor */
            if (!sceKernelAvailableFlexibleMemorySize( &pool ) && pool < OUTSIDE_POOL_FLOOR + ((size_t)n << PAGE_SHIFT))
            {
                if (!told_pool++)
                    fprintf( stderr, "wineps4 vm: the memory outside the PS4's 7 is down to %zu MiB: no more for this "
                             "process (it has %llu MiB), the PS4 keeps %u MiB\n", pool >> 20, backed_flexible >> 6,
                             (unsigned int)(OUTSIDE_POOL_FLOOR >> 20) );
                errno = ENOMEM;
                return 0;
            }
            ret = sceKernelMapNamedFlexibleMemory( &base, (size_t)n << PAGE_SHIFT, PROT_READ | PROT_WRITE, SCE_FIXED,
                                                   "wine" );
        }
        else if (flexible)
        {
            n = want;
            ret = sceKernelMapNamedSystemFlexibleMemory( &base, (size_t)n << PAGE_SHIFT, PROT_READ | PROT_WRITE,
                                                         SCE_FIXED, "wine" );
        }
        else
        {
            if (!(n = take_direct( want, &first ))) { errno = ENOMEM; return 0; }
            ret = sceKernelMapDirectMemory( &base, (size_t)n << PAGE_SHIFT, PROT_READ | PROT_WRITE, SCE_FIXED,
                                            direct_phys( first ), HOST_PAGE );
        }
        if (ret || base != (void *)addr)
        {
            fprintf( stderr, "wineps4 vm: backing %p (%u pages, %s) failed: %#x\n", (void *)addr, n,
                     flexible_only ? "regular flexible" : flexible ? "flexible" : "direct", (unsigned int)ret );
            if (!flexible) give_direct( first, n );
            errno = ENOMEM;
            return 0;
        }
        /* direct memory comes as its last user left it; flexible memory is the kernel's, zero until
         * a page is first touched (written here, every page backed became the console's at once) */
        if (!flexible) memset( base, 0, (size_t)n << PAGE_SHIFT );
        if ((prot & PROT_ALL) != (PROT_READ | PROT_WRITE)) sceKernelMprotect( base, (size_t)n << PAGE_SHIFT, prot & PROT_ALL );
        for (i = 0; i < n; i++)
            page_set( addr + ((uintptr_t)i << PAGE_SHIFT), flexible ? PAGE_FLEXIBLE : PAGE_DIRECT + first + i );
        if (flexible) backed_flexible += n;
        else backed_direct += n;
        addr += (uintptr_t)n << PAGE_SHIFT;
    }
    return 1;
}

/* Direct memory that has to run code on a console where it can't: copied to flexible memory. */
static int move_to_flexible( uintptr_t start, uintptr_t end, int prot )
{
    static char copy[HOST_PAGE];
    uintptr_t addr;

    for (addr = start; addr < end; addr += HOST_PAGE)
    {
        uint32_t state = page_get( addr );
        void *base = (void *)addr;

        if (state < PAGE_DIRECT) continue;
        sceKernelMprotect( base, HOST_PAGE, PROT_READ | PROT_WRITE );
        memcpy( copy, base, HOST_PAGE );
        sceKernelMunmap( base, HOST_PAGE );
        give_direct( state - PAGE_DIRECT, 1 );
        backed_direct--;
        page_set( addr, PAGE_RESERVED );
        if (sceKernelMapNamedSystemFlexibleMemory( &base, HOST_PAGE, PROT_READ | PROT_WRITE, SCE_FIXED, "wine" ) ||
            base != (void *)addr)
        {
            fprintf( stderr, "wineps4 vm: no flexible memory for code at %p\n", (void *)addr );
            errno = ENOMEM;
            return 0;
        }
        memcpy( base, copy, HOST_PAGE );
        sceKernelMprotect( base, HOST_PAGE, prot & PROT_ALL );
        page_set( addr, PAGE_FLEXIBLE );
        backed_flexible++;
    }
    return 1;
}

/* --- can direct memory run code? --------------------------------------------------------------- */

static jmp_buf exec_jump;
static volatile int exec_faulted;

static void exec_fault( int sig )
{
    exec_faulted = sig;
    longjmp( exec_jump, 1 );
}

static void test_direct_exec( void )
{
    static const unsigned char code[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };  /* mov eax, 42; ret */
    struct sigaction sa, old_segv, old_bus;
    void *base = NULL;
    volatile int result = 0;

    if (sceKernelReserveVirtualRange( &base, HOST_PAGE, 0, HOST_PAGE )) return;
    page_set( (uintptr_t)base, PAGE_RESERVED );
    if (!back_range( (uintptr_t)base, (uintptr_t)base + HOST_PAGE, PROT_READ | PROT_WRITE )) return;
    memcpy( base, code, sizeof(code) );
    sceKernelMprotect( base, HOST_PAGE, PROT_READ | PROT_EXEC );

    memset( &sa, 0, sizeof(sa) );
    sa.sa_handler = exec_fault;
    sa.sa_flags = 0x10;  /* FreeBSD's SA_NODEFER: the handler leaves with longjmp */
    sigaction( SIGSEGV, &sa, &old_segv );
    sigaction( 10 /* FreeBSD's SIGBUS */, &sa, &old_bus );
    if (!setjmp( exec_jump )) result = ((int (*)(void))base)();
    sigaction( SIGSEGV, &old_segv, NULL );
    sigaction( 10, &old_bus, NULL );

    if (flexible_only)  /* (the test ran on regular flexible memory: nothing else to go to) */
        fprintf( stderr, "wineps4 vm: regular flexible memory %s code\n", result == 42 ? "runs" : "DOES NOT RUN" );
    else
    {
        direct_exec = result == 42;
        fprintf( stderr, "wineps4 vm: direct memory %s code%s\n", direct_exec ? "runs" : "does not run",
                 direct_exec ? "" : " (code pages go to system flexible memory)" );
    }
    if (result != 42 && exec_faulted) fprintf( stderr, "wineps4 vm: the test faulted with signal %d\n", exec_faulted );
    release_range( (uintptr_t)base, (uintptr_t)base + HOST_PAGE );
}

/* The top of the address space a reservation can have: the highest 1 GiB step below 1 TiB where a
 * reservation lands where it was asked for. Wine's own probe (an address hint, halving) finds 1 TiB,
 * and its top-down searches then try every 64 KiB below that, in vain. */
static uintptr_t address_limit;
static void vm_init( void );

static void find_address_limit( void )
{
    uintptr_t top;

    for (top = (uintptr_t)1 << 40; top > ((uintptr_t)1 << 36); top -= (uintptr_t)1 << 30)
    {
        void *want = (void *)(top - HOST_PAGE), *base = want;
        if (sceKernelReserveVirtualRange( &base, HOST_PAGE, 0, HOST_PAGE )) continue;
        sceKernelMunmap( base, HOST_PAGE );
        if (base == want) break;
    }
    address_limit = top;
    fprintf( stderr, "wineps4 vm: address space reservable up to %#llx\n", (unsigned long long)top );
}

/* A reservation at an address of the kernel's choosing that it refused: without an address the PS4
 * looks only in its own part below 32 GiB (0x400000-0x7ffffc000), where one as big as Chromium's
 * 32 GiB (its memory pools: steamwebhelper's chrome_elf.dll, v00.50) never fits. Then down from the
 * top of the app's part, a GiB at a time, in pieces of 4 GiB at most, each where it was asked for (a
 * piece the kernel puts elsewhere goes again, and so does that try): its base, or NULL. */
static void *reserve_high( size_t size )
{
    const size_t piece_max = (size_t)4 << 30;
    uintptr_t top;

    if (size >= address_limit - APP_AREA_START) return NULL;
    for (top = (address_limit - size) & ~(GIB - 1); top >= APP_AREA_START; top -= GIB)
    {
        size_t done = 0;

        while (done < size)
        {
            const size_t piece = size - done < piece_max ? size - done : piece_max;
            void *want = (void *)(top + done), *base = want;

            if (sceKernelReserveVirtualRange( &base, piece, 0, HOST_PAGE )) break;
            if (base != want)
            {
                sceKernelMunmap( base, piece );
                break;
            }
            done += piece;
        }
        if (done == size) return (void *)top;
        if (done) sceKernelMunmap( (void *)top, done );
    }
    return NULL;
}

/* start + size reserved where asked (MAP_FIXED), in pieces of 4 GiB at most as above */
static int reserve_fixed( uintptr_t start, size_t size )
{
    const size_t piece_max = (size_t)4 << 30;
    size_t done = 0;

    while (done < size)
    {
        const size_t piece = size - done < piece_max ? size - done : piece_max;
        void *want = (void *)(start + done), *base = want;

        if (sceKernelReserveVirtualRange( &base, piece, SCE_FIXED, HOST_PAGE ) || base != want)
        {
            if (done) sceKernelMunmap( (void *)start, done );
            return 0;
        }
        done += piece;
    }
    return 1;
}

/* for ntdll's get_host_addr_space_limit */
void *ps4_vm_address_limit( void )
{
    pthread_once( &vm_once, vm_init );
    return (void *)address_limit;
}

static void vm_init( void )
{
    pthread_mutexattr_t attr;
    const char *env;
    off_t start = 0;
    size_t size = 0;

    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &vm_lock, &attr );
    pthread_mutexattr_destroy( &attr );
    vm_log = (env = getenv( "WINEPS4_VM_LOG" )) && atoi( env );
    if ((env = getenv( "WINEPS4_DIRECT_RESERVE_MB" ))) direct_reserve = (size_t)atoi( env ) << 20;
    if ((flexible_only = ps4_outside_process))
    {
        if ((env = getenv( "WINEPS4_OUTSIDE_MAX_MB" )) && atoi( env ) > 0)
            flexible_max = ((unsigned long long)atoi( env ) << 20) >> PAGE_SHIFT;
        if (sceKernelAvailableFlexibleMemorySize( &size )) size = 0;
        fprintf( stderr, "wineps4 vm: outside the PS4's 7: regular flexible memory only, %llu MiB at most "
                 "(the kernel says %zu MiB of it free)\n", flexible_max >> 6, size >> 20 );
    }
    else
    {
        sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &size );
        fprintf( stderr, "wineps4 vm: direct memory %zu MiB, largest free %zu MiB, %zu MiB kept for the app\n",
                 sceKernelGetDirectMemorySize() >> 20, size >> 20, direct_reserve >> 20 );
    }
    find_address_limit();
    test_direct_exec();
}

static void vm_enter( void )
{
    pthread_once( &vm_once, vm_init );
    pthread_mutex_lock( &vm_lock );
}

static void vm_leave( void )
{
    pthread_mutex_unlock( &vm_lock );
}

/* --- the calls -------------------------------------------------------------------------------- */

/* GlobalMemoryStatusEx's figures (ntdll's get_performance_info): the app's direct memory as the RAM, its
 * largest free run (less what's kept for the app, which programs can't have) and the flexible memory free
 * as what's available. Programs saw none available before: Chromium (Steam's web helper) was in critical
 * memory pressure all the time ("Discarding multiple pages" every 5 s, v00.55); with all of it counted it
 * took more than there was to give (v00.56). */
void ps4_vm_memory_status( unsigned long long *total, unsigned long long *avail )
{
    size_t flexible = 0, size = 0;
    off_t start = 0;

    *total = sceKernelGetDirectMemorySize();
    if (sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &size )) size = 0;
    if (sceKernelAvailableFlexibleMemorySize( &flexible )) flexible = 0;
    *avail = (size > direct_reserve + memory_margin ? size - direct_reserve - memory_margin : 0) + flexible;
    if (*avail > *total) *avail = *total;
}

/* A Chromium program (Steam's web helper) sees the app's free memory less this much: it takes what it is
 * told it may have, and the app's other processes (a game) share it. Told all of it, the web helper grew
 * until the app's direct memory was gone (v00.56, v00.58: it died, then the app froze); told none, it
 * stayed small (v00.55). With 1 GiB less, it is short of memory once the app has under 1.25 GiB free. */
void ps4_vm_set_memory_margin( unsigned long long margin )
{
    memory_margin = margin;
}

/* the largest free run of the app's direct memory, less what's kept for the app: for the desktop's choice
 * of where a new process runs (port/ps4_spawn_desk.c) */
unsigned long long ps4_vm_direct_free( void )
{
    size_t size = 0;
    off_t start = 0;

    if (sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &size )) return 0;
    return size > direct_reserve ? size - direct_reserve : 0;
}

static void *vm_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(HOST_PAGE - 1);
    size_t size = ((uintptr_t)addr + len - start + HOST_PAGE - 1) & ~(size_t)(HOST_PAGE - 1);
    void *ret = MAP_FAILED, *base;

    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (fd != -1 || !(flags & MAP_ANON))
    {
        if (!(flags & MAP_FIXED)) return __real_mmap( addr, len, prot, flags, fd, offset );
        if (!(flags & MAP_SHARED) || ((prot & PROT_EXEC) && start != USD_ADDRESS))
        {
            LOG( "file mapping %p-%p refused: Wine reads it\n", addr, (char *)addr + len );
            errno = ENODEV;
            return MAP_FAILED;
        }
        vm_enter();
        release_range( start, start + size );
        ret = __real_mmap( addr, len, prot, flags, fd, offset );
        LOG( "file mapping %p-%p prot %d shared = %p\n", addr, (char *)addr + len, prot, ret );
        vm_leave();
        return ret;
    }

    vm_enter();
    if (flags & MAP_FIXED)
    {
        if (start + size > ADDR_LIMIT) { errno = ENOMEM; goto done; }
        release_range( start, start + size );
        base = (void *)start;
        if (!reserve_fixed( start, size ))
        {
            fprintf( stderr, "wineps4 vm: fixed reservation %p-%p failed\n", (void *)start, (void *)(start + size) );
            errno = ENOMEM;
            goto done;
        }
    }
    else
    {
        base = (void *)start;  /* a hint: Wine checks where it landed */
        if (sceKernelReserveVirtualRange( &base, size, 0, HOST_PAGE ))
        {
            /* refused (above the console's address space, or taken): Wine looks further down */
            if (start) { errno = EEXIST; goto done; }
            if (!(base = reserve_high( size )))
            {
                fprintf( stderr, "wineps4 vm: no room for %zu MiB of address space, low or high\n", size >> 20 );
                errno = ENOMEM;
                goto done;
            }
            fprintf( stderr, "wineps4 vm: %zu MiB of address space at %p (the kernel had no room below 32 GiB)\n",
                     size >> 20, base );
        }
        if ((uintptr_t)base + size > ADDR_LIMIT)
        {
            sceKernelMunmap( base, size );
            errno = ENOMEM;
            goto done;
        }
    }
    if (!reserve_pages( (uintptr_t)base, (uintptr_t)base + size ))
    {
        fprintf( stderr, "wineps4 vm: no memory for the page table of %p+%#zx\n", base, size );
        errno = ENOMEM;
        release_range( (uintptr_t)base, (uintptr_t)base + size );
        goto done;
    }
    if ((prot & PROT_ALL) && !back_range( (uintptr_t)base, (uintptr_t)base + size, prot ))
    {
        release_range( (uintptr_t)base, (uintptr_t)base + size );
        goto done;
    }
    ret = base;
done:
    LOG( "mmap %p+%#zx prot %d flags %#x = %p (direct %llu MiB, flexible %llu MiB)\n", addr, len, prot, flags,
         ret, backed_direct >> 6, backed_flexible >> 6 );
    if (ret == MAP_FAILED && errno != EEXIST && !vm_log)  /* failures always (an address taken is Wine's search) */
        fprintf( stderr, "wineps4 vm: mmap %p+%#zx prot %d flags %#x failed: %s\n", addr, len, prot, flags,
                 strerror( errno ) );
    vm_leave();
    return ret;
}

static int vm_munmap( void *addr, size_t len )
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(HOST_PAGE - 1);
    uintptr_t end = ((uintptr_t)addr + len + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1);
    int ret;

    if (!len) { errno = EINVAL; return -1; }
    vm_enter();
    ret = release_range( start, end );
    vm_leave();
    LOG( "munmap %p+%#zx = %d\n", addr, len, ret );
    return ret;
}

static int vm_mprotect( void *addr, size_t len, int prot )
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(HOST_PAGE - 1);
    uintptr_t end = ((uintptr_t)addr + len + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1);
    uintptr_t run;
    int ret = 0;

    vm_enter();
    for (run = start; run < end && !ret; )
    {
        uint32_t state = page_get( run ), kind = state >= PAGE_DIRECT ? PAGE_DIRECT : state;
        uintptr_t next = run + HOST_PAGE;

        while (next < end)
        {
            uint32_t s = page_get( next );
            if ((s >= PAGE_DIRECT ? PAGE_DIRECT : s) != kind) break;
            next += HOST_PAGE;
        }
        switch (kind)
        {
        case PAGE_UNTRACKED:
            ret = mprotect( (void *)run, next - run, prot );
            break;
        case PAGE_RESERVED:
            if ((prot & PROT_ALL) && !back_range( run, next, prot )) ret = -1;
            break;
        case PAGE_DIRECT:
            if ((prot & PROT_EXEC) && !direct_exec)
            {
                if (!move_to_flexible( run, next, prot )) ret = -1;
                break;
            }
            /* fall through */
        case PAGE_FLEXIBLE:
            if (sceKernelMprotect( (void *)run, next - run, prot & PROT_ALL )) { errno = EACCES; ret = -1; }
            break;
        }
        run = next;
    }
    vm_leave();
    if (vm_log && (prot & PROT_ALL)) LOG( "mprotect %p+%#zx prot %d = %d\n", addr, len, prot, ret );
    else if (ret) fprintf( stderr, "wineps4 vm: mprotect %p+%#zx prot %d failed: %s\n", addr, len, prot, strerror( errno ) );
    return ret;
}

/* Wine's hints (huge pages, discard, read ahead): nothing the PS4 needs */
static int vm_madvise( void *addr, size_t len, int advice )
{
    return 0;
}

/* --- Sections in the desktop's direct memory ---------------------------------------------------
 *
 * Wine's anonymous sections (CreateFileMapping without a file: Chromium's shared memory, 223 MiB of
 * Steam's web helper in v00.67) were shared memory objects, which the PS4 takes from the flexible
 * memory the app's local processes share: about 1 GiB, and when it is gone a page fault waits for
 * good (v00.63 to v00.66). Now wineserver, in the desktop, puts an unnamed one that a program among
 * the 7 makes in the app's direct memory (4.6 GiB, over 1 GiB of it free then): it allocates and
 * zeroes it (ps4_section_alloc), each program maps it at its views (ps4_vm_map_section, from ntdll's
 * virtual_map_section), and it goes back to the app 2 s after the section's fd has ended (server/
 * fd.c). Named sections (another process opens them, maybe one outside the 7), SEC_RESERVE ones
 * (committed as they are used), the sections of processes outside the 7, which never map direct
 * memory (v00.42: the PS4 came down when the app was suspended), and every one while the app's
 * direct memory is short stay shared memory.
 *
 * Whether a process may map direct memory that another one allocated is the kernel's to say: the
 * desktop writes a token into a page of it, each local process maps that page as it starts and reads
 * it (WINEPS4_SECTION_PROBE: the page's offset and the token), and sections go there once one has
 * read it and written back.
 *
 * v00.69's test on the PS4: every local process's map of the desktop's page was refused
 * (sceKernelMapDirectMemory 0x8002000d), so sections stay shared memory unless it is asked for:
 *
 *  WINEPS4_SECTIONS_DIRECT=1      the test, and sections in direct memory if it passes (Settings >
 *                                 Diagnostics) */

#define SECTION_MAX   ((size_t)256 << 20)  /* the biggest section in direct memory */
#define SECTION_FLOOR ((size_t)640 << 20)  /* what sections leave of the app's largest free run of direct
                                               memory: the app's own (direct_reserve) and a new local
                                               process's (port/ps4_spawn_desk.c's LOW_DIRECT_MEMORY) */
#define PROBE_SEEN    0x5345454eu          /* "SEEN": a local process read the token (its pid above it) */

extern int ps4_pid_local( int pid );  /* (port/ps4_spawn_desk.c) */
int32_t sceKernelEnableDmemAliasing( void );
int32_t sceKernelReleaseDirectMemory( off_t start, size_t len );

static volatile uint64_t *section_probe;  /* the desktop: the token, then what a local process wrote back */
static int sections_mappable;             /* a local process: it read the token */
static unsigned long long section_count, section_bytes;  /* the desktop: the sections in direct memory now */

/* *p, or ~0 if reading it faults */
static uint64_t guarded_read( volatile uint64_t *p )
{
    struct sigaction sa, old_segv, old_bus;
    volatile uint64_t value = ~0ull;

    memset( &sa, 0, sizeof(sa) );
    sa.sa_handler = exec_fault;
    sa.sa_flags = 0x10;  /* FreeBSD's SA_NODEFER: the handler leaves with longjmp */
    sigaction( SIGSEGV, &sa, &old_segv );
    sigaction( 10 /* FreeBSD's SIGBUS */, &sa, &old_bus );
    if (!setjmp( exec_jump )) value = *p;
    sigaction( SIGSEGV, &old_segv, NULL );
    sigaction( 10, &old_bus, NULL );
    return value;
}

/* The desktop, before it starts a program: the page the local processes test */
void ps4_sections_init( void )
{
    const char *env = getenv( "WINEPS4_SECTIONS_DIRECT" );
    struct timespec now;
    char value[48];
    void *page = NULL;
    off_t phys = 0;
    int ret;

    if (!env || strcmp( env, "1" ))
    {
        fprintf( stderr, "wineps4: sections in shared memory (v00.69's test: the PS4 refused the desktop's direct "
                 "memory to the programs)\n" );
        return;
    }
    if ((ret = sceKernelAllocateDirectMemory( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, HOST_PAGE, 0, &phys )))
    {
        fprintf( stderr, "wineps4: sections in shared memory (no direct memory for the test: %#x)\n", (unsigned int)ret );
        return;
    }
    if ((ret = sceKernelMapDirectMemory( &page, HOST_PAGE, PROT_READ | PROT_WRITE, 0, phys, HOST_PAGE )))
    {
        fprintf( stderr, "wineps4: sections in shared memory (the test's page not mapped: %#x)\n", (unsigned int)ret );
        sceKernelReleaseDirectMemory( phys, HOST_PAGE );
        return;
    }
    clock_gettime( CLOCK_MONOTONIC, &now );
    section_probe = page;
    section_probe[0] = ((uint64_t)now.tv_nsec << 32 ^ (uint64_t)now.tv_sec ^ (uint64_t)getpid() << 16) | 1;
    section_probe[1] = 0;
    snprintf( value, sizeof(value), "%llx:%llx", (unsigned long long)phys, (unsigned long long)section_probe[0] );
    setenv( "WINEPS4_SECTION_PROBE", value, 1 );
}

/* A local process as it starts (port/ps4_spawn_desk.c): does the desktop's direct memory map here?
 * It reads the token, and the first to read it writes back, which lets wineserver put sections there. */
void ps4_section_probe( void )
{
    const char *env = getenv( "WINEPS4_SECTION_PROBE" );
    unsigned long long phys, token;
    volatile uint64_t *page;
    void *base = NULL;
    int ret, alias;

    if (!env || sscanf( env, "%llx:%llx", &phys, &token ) != 2) return;
    alias = sceKernelEnableDmemAliasing();  /* (the memory is mapped in the desktop already) */
    if ((ret = sceKernelMapDirectMemory( &base, HOST_PAGE, PROT_READ | PROT_WRITE, 0, (off_t)phys, HOST_PAGE )))
    {
        fprintf( stderr, "wineps4: sections: the desktop's direct memory does not map here (%#x, aliasing %d): "
                 "they stay in shared memory\n", (unsigned int)ret, alias );
        return;
    }
    page = base;
    if (guarded_read( &page[0] ) != token)
    {
        fprintf( stderr, "wineps4: sections: the desktop's direct memory maps here but reads otherwise (aliasing %d): "
                 "they stay in shared memory\n", alias );
        sceKernelMunmap( base, HOST_PAGE );
        return;
    }
    sections_mappable = 1;
    if ((page[1] & 0xffffffff) != PROBE_SEEN)
    {
        page[1] = (uint64_t)getpid() << 32 | PROBE_SEEN;
        fprintf( stderr, "wineps4: sections: the desktop's direct memory maps here (aliasing %d): programs' "
                 "sections go there\n", alias );
    }
    sceKernelMunmap( base, HOST_PAGE );
}

/* wineserver (server/mapping.c): a new anonymous section of process pid in direct memory: 0, with
 * *offset and *alloc_size; -1: shared memory (not tested yet, a process outside the 7, too big, short) */
int ps4_section_alloc( unsigned long long size, int pid, unsigned long long *offset, unsigned long long *alloc_size )
{
    static int told_first, told_short, told_failed;
    const size_t len = (size + HOST_PAGE - 1) & ~(size_t)(HOST_PAGE - 1);
    size_t largest = 0;
    off_t start = 0, phys = 0;
    void *base = NULL;
    int ret;

    if (!section_probe || (section_probe[1] & 0xffffffff) != PROBE_SEEN) return -1;
    if (len > SECTION_MAX || !ps4_pid_local( pid )) return -1;
    if (sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &largest ) ||
        largest < len + SECTION_FLOOR)
    {
        if (!told_short)
            fprintf( stderr, "wineps4: sections: the app's direct memory is down to %zu MiB: new ones in shared memory\n",
                     largest >> 20 );
        told_short = 1;
        return -1;
    }
    if (told_short) fprintf( stderr, "wineps4: sections: in direct memory again (%zu MiB free)\n", largest >> 20 );
    told_short = 0;
    if ((ret = sceKernelAllocateDirectMemory( 0, sceKernelGetDirectMemorySize(), len, HOST_PAGE, 0, &phys )))
    {
        if (!told_failed++)
            fprintf( stderr, "wineps4: sections: no direct memory for %zu KiB (%#x): shared memory\n", len >> 10,
                     (unsigned int)ret );
        return -1;
    }
    /* direct memory comes as its last user left it: zeroed here, before any program sees it */
    if ((ret = sceKernelMapDirectMemory( &base, len, PROT_READ | PROT_WRITE, 0, phys, HOST_PAGE )))
    {
        if (!told_failed++)
            fprintf( stderr, "wineps4: sections: %zu KiB of direct memory not mapped here to be zeroed (%#x): shared "
                     "memory\n", len >> 10, (unsigned int)ret );
        sceKernelReleaseDirectMemory( phys, len );
        return -1;
    }
    memset( base, 0, len );
    sceKernelMunmap( base, len );
    if (!told_first++)
        fprintf( stderr, "wineps4: sections: the first in direct memory (%zu KiB, process %d)\n", len >> 10, pid );
    section_count++;
    section_bytes += len;
    *offset = (unsigned long long)phys;
    *alloc_size = len;
    return 0;
}

/* wineserver: a section's direct memory back to the app (server/fd.c: 2 s after the section's end) */
void ps4_section_release( unsigned long long offset, unsigned long long size )
{
    static int told;
    int ret;

    if ((ret = sceKernelReleaseDirectMemory( (off_t)offset, size )) && !told++)
        fprintf( stderr, "wineps4: sections: direct memory at %#llx (%llu KiB) not given back (%#x)\n", offset,
                 size >> 10, (unsigned int)ret );
    section_count--;
    section_bytes -= size;
}

/* the Performance log's figures: the sections in direct memory now, and their bytes (read without a
 * lock: figures for the eye) */
unsigned long long ps4_sections_direct( unsigned long long *bytes )
{
    *bytes = section_bytes;
    return section_count;
}

static int section_refused( void )
{
    static int told;

    if (!told++)
        fprintf( stderr, "wineps4: a section in the desktop's direct memory not mapped: %s\n",
                 ps4_outside_process ? "a process outside the PS4's 7 maps no direct memory"
                                     : "this process could not map it as it started" );
    errno = EACCES;
    return -1;
}

/* A program's view of a section in the desktop's direct memory (ntdll's virtual_map_section): the
 * view's pages, reserved until now, become a mapping of that memory at offset, which munmap and
 * mprotect then pass to the kernel as they do a file's */
int ps4_vm_map_section( void *addr, size_t len, int prot, unsigned long long offset )
{
    const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(HOST_PAGE - 1);
    const size_t size = ((uintptr_t)addr + len - start + HOST_PAGE - 1) & ~(size_t)(HOST_PAGE - 1);
    void *base = (void *)start;
    int ret;

    if (!sections_mappable) return section_refused();
    vm_enter();
    ret = sceKernelMapDirectMemory( &base, size, prot & (PROT_READ | PROT_WRITE), SCE_FIXED, (off_t)offset, HOST_PAGE );
    if (!ret && base == (void *)start) forget_range( start, start + size );
    else if (!ret) sceKernelMunmap( base, size );
    vm_leave();
    LOG( "section %p+%#zx prot %d = direct memory %#llx (%#x)\n", (void *)start, size, prot, offset, (unsigned int)ret );
    if (!ret && base == (void *)start) return 0;
    fprintf( stderr, "wineps4 vm: a section's view %p+%#zx not mapped from direct memory %#llx: %#x\n", (void *)start,
             size, offset, (unsigned int)ret );
    errno = ENOMEM;
    return -1;
}

/* A copy-on-write view of such a section: its own pages with a copy of the section's (direct memory
 * isn't copied on write) */
int ps4_vm_copy_section( void *addr, size_t len, unsigned long long offset )
{
    const size_t size = (len + HOST_PAGE - 1) & ~(size_t)(HOST_PAGE - 1);
    void *source = NULL;
    int ret;

    if (!sections_mappable) return section_refused();
    if ((ret = sceKernelMapDirectMemory( &source, size, PROT_READ, 0, (off_t)offset, HOST_PAGE )))
    {
        fprintf( stderr, "wineps4 vm: a section's direct memory %#llx not mapped for a copy: %#x\n", offset,
                 (unsigned int)ret );
        errno = ENOMEM;
        return -1;
    }
    if (!(ret = vm_mprotect( addr, len, PROT_READ | PROT_WRITE ))) memcpy( addr, source, len );
    sceKernelMunmap( source, size );
    return ret;
}

/* --- The calls, each marked while it runs ----------------------------------------------------------- */

/* A thread that stands in one of these is seen in the freeze report by its mark (port/ps4_kstate.c):
 * no signal reaches a thread that waits in the kernel (v00.64 to v00.66). */
extern void ps4_call_enter( const char *what );
extern void ps4_call_leave( void );

void *ps4_vm_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
{
    void *ret;

    ps4_call_enter( fd != -1 && (flags & MAP_SHARED) ? "mmap (shared)" : "mmap" );
    ret = vm_mmap( addr, len, prot, flags, fd, offset );
    ps4_call_leave();
    return ret;
}

int ps4_vm_munmap( void *addr, size_t len )
{
    int ret;

    ps4_call_enter( "munmap" );
    ret = vm_munmap( addr, len );
    ps4_call_leave();
    return ret;
}

int ps4_vm_mprotect( void *addr, size_t len, int prot )
{
    int ret;

    ps4_call_enter( "mprotect" );
    ret = vm_mprotect( addr, len, prot );
    ps4_call_leave();
    return ret;
}

int ps4_vm_madvise( void *addr, size_t len, int advice )
{
    int ret;

    ps4_call_enter( "madvise" );
    ret = vm_madvise( addr, len, advice );
    ps4_call_leave();
    return ret;
}
