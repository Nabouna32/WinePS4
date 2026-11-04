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
 *  WINEPS4_VM_LOG=1               log every operation (env.txt), off by default
 *  WINEPS4_DIRECT_RESERVE_MB=n    direct memory left to the rest of the app (default 256) */
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
#include <unistd.h>

int32_t sceKernelReserveVirtualRange( void **addr, size_t len, int32_t flags, size_t alignment );
int32_t sceKernelMunmap( void *addr, size_t len );
int32_t sceKernelMprotect( const void *addr, size_t len, int32_t prot );
int32_t sceKernelMapNamedSystemFlexibleMemory( void **addr, size_t len, int32_t prot, int32_t flags,
                                               const char *name );
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

enum { PAGE_UNTRACKED = 0, PAGE_RESERVED = 1, PAGE_FLEXIBLE = 2, PAGE_DIRECT = 3 /* + direct page */ };

/* direct memory, taken from the system in 64 MiB chunks */
#define CHUNK_PAGES  4096
#define MAX_CHUNKS   80
static off_t chunk_phys[MAX_CHUNKS];
static unsigned int chunk_count, chunk_used;  /* chunk_used: pages handed out of the last chunk */
static uint32_t free_pages[MAX_CHUNKS * CHUNK_PAGES];
static unsigned int free_count;
static size_t direct_reserve = 256u << 20;

static uint32_t *l1[L1_ENTRIES];
static pthread_mutex_t vm_lock;
static pthread_once_t vm_once = PTHREAD_ONCE_INIT;
static int vm_log;
static int direct_exec = 1;  /* direct memory runs code (tested at start) */
static unsigned long long backed_direct, backed_flexible;  /* pages */

#define LOG(...) do { if (vm_log) fprintf( stderr, "wineps4 vm: " __VA_ARGS__ ); } while (0)

static uint32_t page_get( uintptr_t addr )
{
    uint32_t *l2 = l1[addr >> L2_SHIFT];
    return l2 ? l2[(addr >> PAGE_SHIFT) & (L2_PAGES - 1)] : PAGE_UNTRACKED;
}

static int page_set( uintptr_t addr, uint32_t state )
{
    uint32_t **slot = &l1[addr >> L2_SHIFT];

    if (!*slot)
    {
        if (state == PAGE_UNTRACKED) return 1;
        if (!(*slot = calloc( L2_PAGES, sizeof(uint32_t) ))) return 0;
    }
    (*slot)[(addr >> PAGE_SHIFT) & (L2_PAGES - 1)] = state;
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

/* Gives the pages' memory back and unmaps the range, ours or the kernel's. */
/* what this process has backed, for Task Manager (port/ps4_spawn_desk.c; read without the lock: a
 * figure for the eye) */
unsigned long long ps4_vm_backed_bytes( void )
{
    return (backed_direct + backed_flexible) * HOST_PAGE;
}

static int release_range( uintptr_t start, uintptr_t end )
{
    uintptr_t addr;

    for (addr = start; addr < end; addr += HOST_PAGE)
    {
        uint32_t state = page_get( addr );
        if (state == PAGE_UNTRACKED) continue;
        if (state >= PAGE_DIRECT) { give_direct( state - PAGE_DIRECT, 1 ); backed_direct--; }
        else if (state == PAGE_FLEXIBLE) backed_flexible--;
        page_set( addr, PAGE_UNTRACKED );
    }
    return sceKernelMunmap( (void *)start, end - start ) ? -1 : 0;
}

/* Backs reserved pages: zeroed while still inaccessible to everyone else, then given prot. */
static int back_range( uintptr_t start, uintptr_t end, int prot )
{
    int flexible = (prot & PROT_EXEC) && !direct_exec;
    uintptr_t addr = start;

    while (addr < end)
    {
        unsigned int want = (end - addr) >> PAGE_SHIFT, n, i;
        uint32_t first = 0;
        void *base = (void *)addr;
        int ret;

        if (flexible)
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
                     flexible ? "flexible" : "direct", (unsigned int)ret );
            if (!flexible) give_direct( first, n );
            errno = ENOMEM;
            return 0;
        }
        memset( base, 0, (size_t)n << PAGE_SHIFT );
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

    direct_exec = result == 42;
    fprintf( stderr, "wineps4 vm: direct memory %s code%s\n", direct_exec ? "runs" : "does not run",
             direct_exec ? "" : " (code pages go to system flexible memory)" );
    if (!direct_exec && exec_faulted) fprintf( stderr, "wineps4 vm: the test faulted with signal %d\n", exec_faulted );
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
    sceKernelAvailableDirectMemorySize( 0, sceKernelGetDirectMemorySize(), HOST_PAGE, &start, &size );
    fprintf( stderr, "wineps4 vm: direct memory %zu MiB, largest free %zu MiB, %zu MiB kept for the app\n",
             sceKernelGetDirectMemorySize() >> 20, size >> 20, direct_reserve >> 20 );
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

void *ps4_vm_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
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
        if (sceKernelReserveVirtualRange( &base, size, SCE_FIXED, HOST_PAGE ) || base != (void *)start)
        {
            fprintf( stderr, "wineps4 vm: fixed reservation %p-%p failed\n", (void *)start, (void *)(start + size) );
            errno = ENOMEM;
            goto done;
        }
    }
    else
    {
        base = (void *)start;  /* a hint: Wine checks where it landed */
        /* refused (above the console's address space, or taken): Wine looks further down */
        if (sceKernelReserveVirtualRange( &base, size, 0, HOST_PAGE )) { errno = start ? EEXIST : ENOMEM; goto done; }
        if ((uintptr_t)base + size > ADDR_LIMIT)
        {
            sceKernelMunmap( base, size );
            errno = ENOMEM;
            goto done;
        }
    }
    for (uintptr_t a = (uintptr_t)base; a < (uintptr_t)base + size; a += HOST_PAGE)
        if (!page_set( a, PAGE_RESERVED )) { errno = ENOMEM; release_range( (uintptr_t)base, (uintptr_t)base + size ); goto done; }
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

int ps4_vm_munmap( void *addr, size_t len )
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

int ps4_vm_mprotect( void *addr, size_t len, int prot )
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
int ps4_vm_madvise( void *addr, size_t len, int advice )
{
    return 0;
}
