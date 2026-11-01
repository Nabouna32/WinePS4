/*
 * wineps4.drv: the PS4's screen (WinePS4)
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps4drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps4drv);

/* one monitor: the desktop's screen; the taskbar at its bottom is not in the work area */
#define TASKBAR_HEIGHT 60

UINT PS4DRV_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param )
{
    DEVMODEW mode = { .dmSize = sizeof(mode) };
    struct gdi_monitor monitor = { 0 };
    struct pci_id pci_id = { 0 };

    manager->add_gpu( "PlayStation 4", &pci_id, NULL, param );
    manager->add_source( "PS4", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE,
                         NtUserGetSystemDpiForProcess( NULL ), param );

    SetRect( &monitor.rc_monitor, 0, 0, PS4_DISPLAY_WIDTH, PS4_DISPLAY_HEIGHT );
    SetRect( &monitor.rc_work, 0, 0, PS4_DISPLAY_WIDTH, PS4_DISPLAY_HEIGHT - TASKBAR_HEIGHT );
    manager->add_monitor( &monitor, param );

    mode.dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
    mode.dmDisplayOrientation = DMDO_DEFAULT;
    mode.dmBitsPerPel = 32;
    mode.dmPelsWidth = PS4_DISPLAY_WIDTH;
    mode.dmPelsHeight = PS4_DISPLAY_HEIGHT;
    mode.dmDisplayFrequency = 60;
    manager->add_modes( &mode, 1, &mode, param );
    return STATUS_SUCCESS;
}
