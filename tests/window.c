/* WinePS4's window test: a window with text, a button, a text box and a check box; the button
 * opens a message box. Tests the display driver (wineps4.drv), fonts and the desktop's input.
 * Built by tests/build.sh (64-bit). */
#include <windows.h>
#include <stdio.h>

#define ID_BUTTON 1
#define ID_EDIT   2
#define ID_CHECK  3

static int clicks;

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT font = GetStockObject( DEFAULT_GUI_FONT );
        HWND child;
        child = CreateWindowW( L"BUTTON", L"Say hello", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               24, 150, 160, 36, hwnd, (HMENU)ID_BUTTON, NULL, NULL );
        SendMessageW( child, WM_SETFONT, (WPARAM)font, TRUE );
        child = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", L"Type here (Triangle)", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 24, 205, 360, 30, hwnd, (HMENU)ID_EDIT, NULL, NULL );
        SendMessageW( child, WM_SETFONT, (WPARAM)font, TRUE );
        child = CreateWindowW( L"BUTTON", L"A check box", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               24, 250, 200, 28, hwnd, (HMENU)ID_CHECK, NULL, NULL );
        SendMessageW( child, WM_SETFONT, (WPARAM)font, TRUE );
        printf( "window test: window created\n" );
        fflush( stdout );
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BUTTON)
        {
            WCHAR text[256], line[400];
            GetDlgItemTextW( hwnd, ID_EDIT, text, 256 );
            clicks++;
            swprintf( line, 400, L"Hello from Windows on the PS4!\n\nThe text box says: %s\nButton pressed %d time(s).", text, clicks );
            printf( "window test: button pressed (%d)\n", clicks );
            fflush( stdout );
            MessageBoxW( hwnd, line, L"WinePS4", MB_OK | MB_ICONINFORMATION );
            InvalidateRect( hwnd, NULL, TRUE );
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT rc = { 24, 20, 600, 60 };
        HFONT big = CreateFontW( 32, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Tahoma" );
        HFONT old = SelectObject( hdc, big );
        WCHAR line[64];
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, RGB( 0, 90, 180 ));
        DrawTextW( hdc, L"Windows on the PS4", -1, &rc, DT_LEFT | DT_SINGLELINE );
        SelectObject( hdc, GetStockObject( DEFAULT_GUI_FONT ));
        DeleteObject( big );
        SetTextColor( hdc, RGB( 40, 40, 40 ));
        rc.top = 70; rc.bottom = 140;
        DrawTextW( hdc, L"This window is drawn by Wine's win32u and shown by the WinePS4 desktop.\n"
                        L"Click the button, tick the box, drag the title bar.", -1, &rc, DT_LEFT | DT_WORDBREAK );
        swprintf( line, 64, L"Clicks: %d", clicks );
        TextOutW( hdc, 220, 160, line, lstrlenW( line ));
        SelectObject( hdc, old );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_DESTROY:
        printf( "window test: window closed\n" );
        fflush( stdout );
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show )
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;

    printf( "window test: starting\n" );
    fflush( stdout );
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WinePS4Test";
    if (!RegisterClassW( &wc ))
    {
        printf( "window test: RegisterClass failed (%lu)\n", GetLastError() );
        return 1;
    }
    hwnd = CreateWindowW( L"WinePS4Test", L"WinePS4 window test", WS_OVERLAPPEDWINDOW,
                          200, 150, 640, 360, NULL, NULL, inst, NULL );
    if (!hwnd)
    {
        printf( "window test: CreateWindow failed (%lu)\n", GetLastError() );
        return 1;
    }
    ShowWindow( hwnd, SW_SHOW );
    UpdateWindow( hwnd );
    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    printf( "window test: done\n" );
    return 0;
}
