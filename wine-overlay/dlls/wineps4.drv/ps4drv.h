/*
 * wineps4.drv Unix side (WinePS4)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_PS4DRV_H
#define __WINE_PS4DRV_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <pthread.h>
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"

#include "unixlib.h"
#include "ps4_display.h"

/* display.c */
extern UINT PS4DRV_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param );

/* window.c */
extern BOOL PS4DRV_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect,
                                        struct window_surface **surface );
extern void PS4DRV_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                     const struct window_rects *new_rects, struct window_surface *surface );
extern void PS4DRV_DestroyWindow( HWND hwnd );
extern void PS4DRV_SetWindowText( HWND hwnd, LPCWSTR text );
extern void PS4DRV_ActivateWindow( HWND hwnd, HWND previous );

/* ps4drv_main.c: the desktop's socket */
extern BOOL ps4drv_send( const struct ps4_display_header *header, const void *data, UINT data_size );
extern BOOL ps4drv_send_rows( const struct ps4_display_header *header, const void *bits, UINT stride );

#endif /* __WINE_PS4DRV_H */
