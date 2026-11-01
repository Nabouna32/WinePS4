/* WinePS4: what the window functions return, step by step (a console program, so every step
 * prints even if no window shows). Built by tests/build.sh. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_CREATE) printf( "  WM_CREATE arrived\n" );
    if (msg == WM_PAINT) printf( "  WM_PAINT arrived\n" );
    if (msg == WM_DESTROY) PostQuitMessage( 0 );
    fflush( stdout );
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int main(void)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd, desktop;
    ATOM atom;
    MSG msg;
    DWORD start;
    int got = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "wintest: GetSystemMetrics(SM_CXSCREEN) = %d, SM_CYSCREEN = %d\n",
            GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ));
    SetLastError( 0 );
    desktop = GetDesktopWindow();
    printf( "wintest: GetDesktopWindow = %p (error %lu)\n", desktop, GetLastError() );
    printf( "wintest: GetForegroundWindow = %p\n", GetForegroundWindow() );

    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleW( NULL );
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WinTest";
    SetLastError( 0 );
    atom = RegisterClassW( &wc );
    printf( "wintest: RegisterClass = %#x (error %lu)\n", atom, GetLastError() );

    SetLastError( 0 );
    hwnd = CreateWindowW( L"WinTest", L"wintest", WS_OVERLAPPEDWINDOW, 300, 200, 500, 300,
                          NULL, NULL, wc.hInstance, NULL );
    printf( "wintest: CreateWindow = %p (error %lu), IsWindow = %d\n", hwnd, GetLastError(), IsWindow( hwnd ));
    if (!hwnd) return 1;
    printf( "wintest: ShowWindow = %d\n", ShowWindow( hwnd, SW_SHOW ));
    printf( "wintest: UpdateWindow = %d, visible %d\n", UpdateWindow( hwnd ), IsWindowVisible( hwnd ));

    start = GetTickCount();
    while (GetTickCount() - start < 8000)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (got++ < 20) printf( "  message %#x for %p\n", msg.message, msg.hwnd );
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 50 );
    }
    printf( "wintest: %d messages in 8 s; DestroyWindow = %d\n", got, DestroyWindow( hwnd ));
    return 0;
}
