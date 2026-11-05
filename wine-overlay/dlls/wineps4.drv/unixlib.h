/*
 * wineps4.drv: the PS4 display and audio driver (WinePS4)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_PS4DRV_UNIXLIB_H
#define __WINE_PS4DRV_UNIXLIB_H

#include <stdarg.h>
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

enum ps4drv_unix_func
{
    /* 0 to 36: the audio driver's, in mmdevapi's order (audio.c, ../mmdevapi/unixlib.h) */
    ps4drv_unix_func_init = 37,
    ps4drv_unix_func_count,
};

#define PS4DRV_UNIX_CALL(func, params) WINE_UNIX_CALL(ps4drv_unix_func_ ## func, params)

#endif /* __WINE_PS4DRV_UNIXLIB_H */
