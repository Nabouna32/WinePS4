/*
 * wineps4.drv entry point (WinePS4)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "unixlib.h"
#include "winbase.h"

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls( instance );
    if (__wine_init_unix_call()) return FALSE;
    return !PS4DRV_UNIX_CALL( init, NULL );
}
