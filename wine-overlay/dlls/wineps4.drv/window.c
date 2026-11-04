/*
 * wineps4.drv: windows (WinePS4)
 *
 * Every top-level window gets a surface of its own; what is drawn on it goes to the desktop when
 * win32u flushes it. Child windows draw on their top-level's surface.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps4drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps4drv);

static BOOL is_toplevel( HWND hwnd )
{
    HWND desktop = NtUserGetDesktopWindow();
    return hwnd != desktop && NtUserGetAncestor( hwnd, GA_PARENT ) == desktop;
}

/* --- surfaces ------------------------------------------------------------------------------------ */

static BOOL ps4_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                               const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                               const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_PIXELS, .hwnd = (UINT_PTR)surface->hwnd };
    int width = color_info->bmiHeader.biWidth, height = abs( color_info->bmiHeader.biHeight );

    msg.left   = max( rect->left + dirty->left, 0 );
    msg.top    = max( rect->top + dirty->top, 0 );
    msg.right  = min( rect->left + dirty->right, width );
    msg.bottom = min( rect->top + dirty->bottom, height );
    msg.width  = width;
    msg.height = height;
    if (msg.left >= msg.right || msg.top >= msg.bottom) return TRUE;
    TRACE( "hwnd %p rows %d-%d cols %d-%d\n", surface->hwnd, msg.top, msg.bottom, msg.left, msg.right );
    ps4drv_send_rows( &msg, color_bits, width * 4 );
    return TRUE;
}

static void ps4_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
}

static void ps4_surface_destroy( struct window_surface *surface )
{
}

static const struct window_surface_funcs ps4_surface_funcs =
{
    ps4_surface_set_clip,
    ps4_surface_flush,
    ps4_surface_destroy
};

BOOL PS4DRV_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect,
                                 struct window_surface **surface )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *previous = *surface;
    int width = surface_rect->right - surface_rect->left, height = surface_rect->bottom - surface_rect->top;

    TRACE( "hwnd %p layered %u rect %s\n", hwnd, layered, wine_dbgstr_rect( surface_rect ));

    /* the same size as before (create_window_surface passes it only then) */
    if (previous && previous->funcs == &ps4_surface_funcs) return TRUE;
    if (previous) window_surface_release( previous );
    *surface = NULL;
    /* the desktop window, message-only windows: nothing to show */
    if (!is_toplevel( hwnd )) return TRUE;

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height;  /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;
    *surface = window_surface_create( sizeof(**surface), &ps4_surface_funcs, hwnd, surface_rect, info, 0 );
    return TRUE;
}

/* --- windows ------------------------------------------------------------------------------------- */

static void send_title( HWND hwnd, const WCHAR *text )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_TITLE, .hwnd = (UINT_PTR)hwnd };
    WCHAR buffer[256];
    char utf8[1024];
    DWORD len = 0;

    if (!text)
    {
        NtUserInternalGetWindowText( hwnd, buffer, ARRAY_SIZE(buffer) );
        text = buffer;
    }
    RtlUnicodeToUTF8N( utf8, sizeof(utf8) - 1, &len, text, wcslen( text ) * sizeof(WCHAR) );
    ps4drv_send( &msg, utf8, len );
}

void PS4DRV_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_WINDOW, .hwnd = (UINT_PTR)hwnd };
    DWORD style, ex_style;

    TRACE( "hwnd %p rects %s flags %08x surface %p\n", hwnd, debugstr_window_rects( new_rects ), swp_flags, surface );
    if (!is_toplevel( hwnd )) return;

    style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
    ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    if ((style & WS_VISIBLE) && surface) msg.flags |= PS4_WINDOW_VISIBLE;
    if (ex_style & WS_EX_TOPMOST) msg.flags |= PS4_WINDOW_TOPMOST;
    if (ex_style & WS_EX_LAYERED) msg.flags |= PS4_WINDOW_LAYERED;
    if ((ex_style & WS_EX_TOOLWINDOW) || (!(style & WS_CAPTION) && (style & WS_POPUP)) ||
        NtUserGetWindowRelative( hwnd, GW_OWNER ))
        msg.flags |= PS4_WINDOW_POPUP;
    if (style & WS_MAXIMIZE) msg.flags |= PS4_WINDOW_MAXIMIZED;
    if (style & WS_MINIMIZE) msg.flags |= PS4_WINDOW_MINIMIZED;
    if (style & WS_MINIMIZEBOX) msg.flags |= PS4_WINDOW_CAN_MINIMIZE;
    if (style & WS_MAXIMIZEBOX) msg.flags |= PS4_WINDOW_CAN_MAXIMIZE;
    msg.left   = new_rects->visible.left;
    msg.top    = new_rects->visible.top;
    msg.right  = new_rects->visible.right;
    msg.bottom = new_rects->visible.bottom;
    msg.client_left   = new_rects->client.left;
    msg.client_top    = new_rects->client.top;
    msg.client_right  = new_rects->client.right;
    msg.client_bottom = new_rects->client.bottom;
    /* the title bar Windows draws, when the program left it to Windows (not one that draws its own
     * over it, as browsers do: their client area takes the window's top) */
    if ((style & WS_CAPTION) == WS_CAPTION && !(style & WS_MINIMIZE))
    {
        TITLEBARINFO info = { .cbSize = sizeof(info) };

        if (NtUserGetTitleBarInfo( hwnd, &info ) && !(info.rgstate[0] & STATE_SYSTEM_INVISIBLE) &&
            info.rcTitleBar.bottom > info.rcTitleBar.top && new_rects->client.top >= info.rcTitleBar.bottom)
        {
            msg.flags |= PS4_WINDOW_CAPTION;
            msg.caption_left   = info.rcTitleBar.left;
            msg.caption_top    = info.rcTitleBar.top;
            msg.caption_right  = info.rcTitleBar.right;
            msg.caption_bottom = info.rcTitleBar.bottom;
        }
    }
    if (surface)
    {
        msg.width  = surface->rect.right - surface->rect.left;
        msg.height = surface->rect.bottom - surface->rect.top;
    }
    if (swp_flags & SWP_SHOWWINDOW) send_title( hwnd, NULL );
    ps4drv_send( &msg, NULL, 0 );
}

void PS4DRV_SetWindowText( HWND hwnd, LPCWSTR text )
{
    if (is_toplevel( hwnd )) send_title( hwnd, text );
}

void PS4DRV_ActivateWindow( HWND hwnd, HWND previous )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_RAISE, .hwnd = (UINT_PTR)hwnd };
    if (is_toplevel( hwnd )) ps4drv_send( &msg, NULL, 0 );
}

/* WinePS4's own command from the desktop's taskbar: the window to the front, restored if it was
 * minimized (0: done; -1: Windows' own handling) */
LRESULT PS4DRV_SysCommand( HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos )
{
    if ((wparam & 0xfff0) != PS4_SC_ACTIVATE) return -1;
    if (NtUserGetWindowLongW( hwnd, GWL_STYLE ) & WS_MINIMIZE) NtUserShowWindow( hwnd, SW_RESTORE );
    NtUserSetForegroundWindow( hwnd );
    return 0;
}

void PS4DRV_DestroyWindow( HWND hwnd )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_DESTROY, .hwnd = (UINT_PTR)hwnd };
    if (is_toplevel( hwnd )) ps4drv_send( &msg, NULL, 0 );
}
