/* WinePS4: how Windows programs' windows reach the desktop, and its input reaches them.
 *
 * Windows: every program process's display driver (wineps4.drv, dlls/wineps4.drv in the Wine tree)
 * connects to the desktop's socket and sends its top-level windows: where they are, whether they
 * show, their title, and the pixels that changed. The desktop keeps a copy of each window and draws
 * them over its own (shell/winwindows.cpp).
 *
 * Input: the desktop writes the PS4's mouse, pad and keyboard as records into a pipe the wineserver
 * reads in its own loop (server/queue.c), which queues them like a PC's hardware input: the server
 * knows the windows, so it picks the one under the cursor or with the focus.
 */
#ifndef WINEPS4_DISPLAY_H
#define WINEPS4_DISPLAY_H

#include <stdint.h>

#define PS4_DISPLAY_SOCKET "/data/WinePS4/.display"
#define PS4_DISPLAY_WIDTH  1920
#define PS4_DISPLAY_HEIGHT 1080

/* --- program -> desktop (the socket) ------------------------------------------------------------- */

enum ps4_display_msg
{
    PS4_DISPLAY_HELLO = 1,  /* pid: a program connected */
    PS4_DISPLAY_WINDOW,     /* where a window is, if it shows */
    PS4_DISPLAY_PIXELS,     /* changed pixels: rows of (right - left) * 4 bytes follow */
    PS4_DISPLAY_TITLE,      /* UTF-8 title follows (size - header) */
    PS4_DISPLAY_DESTROY,    /* the window is gone */
    PS4_DISPLAY_RAISE,      /* the window was activated: on top */
};

#define PS4_WINDOW_VISIBLE  0x01
#define PS4_WINDOW_TOPMOST  0x02
#define PS4_WINDOW_LAYERED  0x04  /* pixels carry alpha */
#define PS4_WINDOW_POPUP    0x08  /* menus, tooltips, ...: no taskbar button */

struct ps4_display_header
{
    uint32_t type;
    uint32_t size;      /* of the whole message, header included */
    uint64_t hwnd;
    int32_t  pid;
    uint32_t flags;     /* PS4_WINDOW_* (WINDOW) */
    int32_t  left, top, right, bottom;  /* WINDOW: the surface on the screen; PIXELS: the rect in it */
    int32_t  width, height;             /* WINDOW: the surface size */
};

/* --- desktop -> wineserver (the pipe) ------------------------------------------------------------ */

enum ps4_input_type
{
    PS4_INPUT_MOUSE = 1,  /* x, y in screen pixels (always absolute), flags = MOUSEEVENTF_*, data = wheel */
    PS4_INPUT_KEY,        /* vkey + scan, flags = KEYEVENTF_* (KEYEVENTF_UNICODE: scan is the character) */
};

struct ps4_input_record
{
    uint32_t type;
    int32_t  x, y;
    uint32_t flags;
    int32_t  data;
    uint16_t vkey, scan;
};

#ifdef __cplusplus
extern "C" {
#endif
/* the desktop's side: queues a record for the wineserver (server/queue.c); 0 if it could not */
int ps4_server_input( const struct ps4_input_record *record );
#ifdef __cplusplus
}
#endif

#endif
