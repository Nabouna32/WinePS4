/* WinePS4's first test program: a 64-bit Windows console program touching the basics a game needs.
 * Its output goes to /data/WinePS4/wine.log. Build: tests/build.sh (mingw). */
#include <windows.h>
#include <stdio.h>

static int failures;

static void check( BOOL ok, const char *what )
{
    printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
    if (!ok) failures++;
    fflush( stdout );
}

static volatile LONG veh_calls;

static LONG CALLBACK handler( EXCEPTION_POINTERS *e )
{
    veh_calls++;
    if (e->ExceptionRecord->ExceptionCode == 0xE0000001) return EXCEPTION_CONTINUE_EXECUTION;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)
    {
        e->ContextRecord->Rip += 2;  /* skip the ud2 */
        e->ContextRecord->Rax = 0x1234;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        e->ContextRecord->Rip += 3;  /* skip "mov (%rcx),%eax" (8b 01 + nop) */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI worker( void *arg )
{
    volatile ULONGLONG sum = 0;
    ULONGLONG i;
    for (i = 0; i < 50000000; i++) sum += i ^ (ULONG_PTR)arg;
    return (DWORD)(sum & 0x7fffffff) | 1;
}

int main( int argc, char **argv )
{
    SYSTEM_INFO si;
    OSVERSIONINFOA ver = { sizeof(ver) };
    MEMORYSTATUSEX mem = { sizeof(mem) };
    LARGE_INTEGER freq, t0, t1;
    char path[MAX_PATH], buf[64];
    HANDLE threads[4], file;
    DWORD written, i, code;
    BYTE *block;
    LONG_PTR rax;

    printf( "Hello from Windows on the PS4!\n" );
    printf( "command line: %s\n", GetCommandLineA() );
    GetModuleFileNameA( NULL, path, sizeof(path) );
    printf( "exe: %s\n", path );
    GetCurrentDirectoryA( sizeof(path), path );
    printf( "current directory: %s\n", path );
    GetVersionExA( &ver );
    printf( "Windows %lu.%lu build %lu\n", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber );
    GetSystemInfo( &si );
    printf( "%lu processors, page size %lu, granularity %lu, user space %p-%p\n", si.dwNumberOfProcessors,
            si.dwPageSize, si.dwAllocationGranularity, si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress );
    GlobalMemoryStatusEx( &mem );
    printf( "memory: %llu MiB total, %llu MiB free\n", mem.ullTotalPhys >> 20, mem.ullAvailPhys >> 20 );
    fflush( stdout );

    block = VirtualAlloc( NULL, 64 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    if (block)
    {
        for (i = 0; i < (64 << 20); i += 4096) block[i] = (BYTE)i;
        check( block[4096 * 7] == (BYTE)(4096 * 7), "VirtualAlloc 64 MiB and touch every page" );
        check( VirtualFree( block, 0, MEM_RELEASE ), "VirtualFree" );
    }
    else check( FALSE, "VirtualAlloc 64 MiB" );

    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < 4; i++) threads[i] = CreateThread( NULL, 0, worker, (void *)(ULONG_PTR)i, 0, NULL );
    code = WaitForMultipleObjects( 4, threads, TRUE, 20000 );
    QueryPerformanceCounter( &t1 );
    check( code == WAIT_OBJECT_0, "4 threads ran and were waited for" );
    for (i = 0; i < 4; i++)
    {
        DWORD exit_code = 0;
        GetExitCodeThread( threads[i], &exit_code );
        if (!exit_code) check( FALSE, "thread exit code" );
        CloseHandle( threads[i] );
    }
    printf( "threads took %.1f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart );

    QueryPerformanceCounter( &t0 );
    Sleep( 100 );
    QueryPerformanceCounter( &t1 );
    printf( "Sleep(100) took %.1f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart );

    file = CreateFileA( "C:\\wineps4-test.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    check( file != INVALID_HANDLE_VALUE, "CreateFile C:\\wineps4-test.txt" );
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile( file, "WinePS4", 7, &written, NULL );
        SetFilePointer( file, 0, NULL, FILE_BEGIN );
        memset( buf, 0, sizeof(buf) );
        ReadFile( file, buf, sizeof(buf) - 1, &written, NULL );
        check( !strcmp( buf, "WinePS4" ), "file written and read back" );
        CloseHandle( file );
        DeleteFileA( "C:\\wineps4-test.txt" );
    }

    AddVectoredExceptionHandler( 1, handler );
    RaiseException( 0xE0000001, 0, 0, NULL );
    check( veh_calls == 1, "RaiseException reaches a vectored handler and continues" );
    __asm__ volatile( "xor %%eax, %%eax\n\tud2" : "=a" (rax) );
    check( rax == 0x1234 && veh_calls == 2, "ud2 becomes EXCEPTION_ILLEGAL_INSTRUCTION, context change kept" );
    __asm__ volatile( "xor %%ecx, %%ecx\n\tmovl (%%rcx), %%eax\n\tnop" : : : "rax", "rcx", "memory" );
    check( veh_calls == 3, "null read becomes EXCEPTION_ACCESS_VIOLATION and resumes" );

    printf( "== %s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures );
    fflush( stdout );
    return failures;
}
