/*
 * wineps4.drv: the PS4 display driver (WinePS4)
 *
 * The windows of a Windows program are shown by the WinePS4 desktop, a separate process: this
 * driver sends it each top-level window's place and pixels over a Unix socket (see
 * port/include/ps4_display.h). Input does not come through here: the desktop gives it to the
 * wineserver, which knows the windows.
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps4drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps4drv);

static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;
static int display_fd = -1;
static BOOL display_failed;

static BOOL write_all( const void *data, size_t size )
{
    const char *ptr = data;

    while (size)
    {
        ssize_t ret = write( display_fd, ptr, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

/* with socket_mutex held */
static BOOL connect_display(void)
{
    struct sockaddr_un addr;
    struct ps4_display_header hello = { .type = PS4_DISPLAY_HELLO, .size = sizeof(hello) };

    if (display_fd != -1) return TRUE;
    if (display_failed) return FALSE;
    if ((display_fd = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1) goto failed;
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, PS4_DISPLAY_SOCKET );
#ifdef HAVE_STRUCT_SOCKADDR_UN_SUN_LEN
    addr.sun_len = sizeof(addr);
#endif
    if (connect( display_fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1) goto failed;
    hello.pid = getpid();
    if (!write_all( &hello, sizeof(hello) )) goto failed;
    TRACE( "connected to the desktop\n" );
    return TRUE;

failed:
    ERR( "cannot reach the WinePS4 desktop at %s: %s\n", PS4_DISPLAY_SOCKET, strerror( errno ));
    if (display_fd != -1) close( display_fd );
    display_fd = -1;
    display_failed = TRUE;  /* windows stay unseen, the program goes on */
    return FALSE;
}

static void lost_display(void)
{
    ERR( "lost the WinePS4 desktop: %s\n", strerror( errno ));
    close( display_fd );
    display_fd = -1;
    display_failed = TRUE;
}

BOOL ps4drv_send( const struct ps4_display_header *header, const void *data, UINT data_size )
{
    struct ps4_display_header msg = *header;
    BOOL ret = FALSE;

    msg.size = sizeof(msg) + data_size;
    msg.pid = getpid();
    pthread_mutex_lock( &socket_mutex );
    if (connect_display())
    {
        if (write_all( &msg, sizeof(msg) ) && (!data_size || write_all( data, data_size ))) ret = TRUE;
        else lost_display();
    }
    pthread_mutex_unlock( &socket_mutex );
    return ret;
}

/* PIXELS: the rows of header->left..right, top..bottom from a top-down 32-bit bitmap */
BOOL ps4drv_send_rows( const struct ps4_display_header *header, const void *bits, UINT stride )
{
    struct ps4_display_header msg = *header;
    UINT row_size = (header->right - header->left) * 4;
    const char *row = (const char *)bits + header->top * stride + header->left * 4;
    BOOL ret = FALSE;
    int y;

    msg.size = sizeof(msg) + row_size * (header->bottom - header->top);
    msg.pid = getpid();
    pthread_mutex_lock( &socket_mutex );
    if (connect_display())
    {
        ret = write_all( &msg, sizeof(msg) );
        for (y = header->top; ret && y < header->bottom; y++, row += stride) ret = write_all( row, row_size );
        if (!ret) lost_display();
    }
    pthread_mutex_unlock( &socket_mutex );
    return ret;
}

/* PIXELS of a Vulkan frame (vulkan.c): rows of (right - left) * 4 bytes from bits, row_bytes apart, the frame's
 * top-left at left, top; one write when its rows are packed */
BOOL ps4drv_send_frame( const struct ps4_display_header *header, const void *bits, UINT row_bytes )
{
    struct ps4_display_header msg = *header;
    UINT row_size = (header->right - header->left) * 4, rows = header->bottom - header->top, y;
    const char *row = bits;
    BOOL ret = FALSE;

    msg.size = sizeof(msg) + row_size * rows;
    msg.pid = getpid();
    pthread_mutex_lock( &socket_mutex );
    if (connect_display())
    {
        ret = write_all( &msg, sizeof(msg) );
        if (ret && row_bytes == row_size) ret = write_all( row, (size_t)row_size * rows );
        else for (y = 0; ret && y < rows; y++, row += row_bytes) ret = write_all( row, row_size );
        if (!ret) lost_display();
    }
    pthread_mutex_unlock( &socket_mutex );
    return ret;
}

static const struct user_driver_funcs ps4drv_funcs =
{
    .pActivateWindow = PS4DRV_ActivateWindow,
    .pCreateWindowSurface = PS4DRV_CreateWindowSurface,
    .pDestroyWindow = PS4DRV_DestroyWindow,
    .pNotifyIMEStatus = PS4DRV_NotifyIMEStatus,
    .pSetIMECompositionRect = PS4DRV_SetIMECompositionRect,
    .pSetWindowText = PS4DRV_SetWindowText,
    .pSysCommand = PS4DRV_SysCommand,
    .pUpdateDisplayDevices = PS4DRV_UpdateDisplayDevices,
    .pVulkanInit = PS4DRV_VulkanInit,
    .pWindowPosChanged = PS4DRV_WindowPosChanged,
};

/* the unix call table is audio.c's: mmdevapi's calls first, then this one */
NTSTATUS ps4drv_unix_init( void *arg )
{
    __wine_set_user_driver( &ps4drv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;
}
