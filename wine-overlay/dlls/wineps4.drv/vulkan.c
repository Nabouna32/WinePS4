/*
 * wineps4.drv's Vulkan (WinePS4): programs' Vulkan, and Direct3D through DXVK, on the PS4's GPU (RADV, linked into
 * the app), shown in their windows on the desktop
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * A program's process has no screen of its own (the desktop has VideoOut), so a window's surface is a headless one
 * of ps4-kit's window system (gpu/ps4_vk_wsi.cpp), in its window mode: its swapchain images are the window's size,
 * in memory the CPU reads, and each present hands the GPU's finished frame to present_frame below, which sends it to
 * the desktop as the window's pixels, as the window's GDI surface sends its own.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps4drv.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* ps4-kit's window system: frames to this instead of VideoOut; its log lines (swapchains made, frames a second) */
extern void ps4kit_set_window_present( void (*hook)( unsigned long long surface, const void *pixels, unsigned width,
                                                     unsigned height, unsigned row_bytes, int bgra ) );
extern void ps4kit_set_log( void (*log)( const char *line ) );

/* Settings > Diagnostics > Vulkan log (WINEPS4_VULKAN_LOG=0: none) */
static void vulkan_log( const char *line )
{
    size_t len = strlen( line );

    while (len && line[len - 1] == '\n') len--;
    fprintf( stderr, "wineps4 vulkan: %.*s\n", (int)len, line );
}

struct ps4_client_surface
{
    struct client_surface client;
    unsigned long long    host;   /* its headless surface (the swapchain's, in present_frame) */
    struct list           entry;  /* in surfaces */
};

static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list surfaces = LIST_INIT( surfaces );

static void ps4drv_client_surface_destroy( struct client_surface *client )
{
    struct ps4_client_surface *surface = CONTAINING_RECORD( client, struct ps4_client_surface, client );

    pthread_mutex_lock( &surfaces_lock );
    list_remove( &surface->entry );
    pthread_mutex_unlock( &surfaces_lock );
}

static void ps4drv_client_surface_detach( struct client_surface *client )
{
}

static void ps4drv_client_surface_update( struct client_surface *client )
{
}

/* (the frame went to the desktop in present_frame, as the swapchain presented it) */
static void ps4drv_client_surface_present( struct client_surface *client, HDC hdc )
{
}

static const struct client_surface_funcs ps4drv_client_surface_funcs =
{
    .destroy = ps4drv_client_surface_destroy,
    .detach = ps4drv_client_surface_detach,
    .update = ps4drv_client_surface_update,
    .present = ps4drv_client_surface_present,
};

/* A frame the GPU has drawn for a surface (ps4-kit's window system, in vkQueuePresentKHR): into its window's
 * client area, on the desktop's copy of the top-level window it is in */
static void present_frame( unsigned long long host, const void *pixels, unsigned width, unsigned height,
                           unsigned row_bytes, int bgra )
{
    struct ps4_display_header msg = { .type = PS4_DISPLAY_PIXELS };
    struct ps4_client_surface *surface;
    RECT window, client;
    HWND hwnd = 0, top;
    UINT dpi;

    pthread_mutex_lock( &surfaces_lock );
    LIST_FOR_EACH_ENTRY( surface, &surfaces, struct ps4_client_surface, entry )
        if (surface->host == host)
        {
            hwnd = surface->client.hwnd;
            break;
        }
    pthread_mutex_unlock( &surfaces_lock );
    if (!hwnd || !(top = NtUserGetAncestor( hwnd, GA_ROOT ))) return;

    /* where its client area is in the top-level window's surface (its whole window: wineps4.drv's surfaces are) */
    dpi = NtUserGetDpiForWindow( hwnd );
    if (!NtUserGetWindowRect( top, &window, dpi ) || !NtUserGetClientRect( hwnd, &client, dpi )) return;
    NtUserMapWindowPoints( hwnd, 0, (POINT *)&client, 2, dpi );
    msg.hwnd   = (UINT_PTR)top;
    msg.width  = window.right - window.left;
    msg.height = window.bottom - window.top;
    msg.left   = max( client.left - window.left, 0 );
    msg.top    = max( client.top - window.top, 0 );
    msg.right  = min( msg.left + (int)width, msg.width );
    msg.bottom = min( msg.top + (int)height, msg.height );
    if (!bgra) msg.flags = PS4_PIXELS_RGBA;
    if (msg.right <= msg.left || msg.bottom <= msg.top) return;
    ps4drv_send_frame( &msg, pixels, row_bytes );
}

static VkResult PS4DRV_vulkan_surface_create( HWND hwnd, const struct vulkan_instance *instance, VkSurfaceKHR *handle,
                                              struct client_surface **client )
{
    VkHeadlessSurfaceCreateInfoEXT info = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    struct ps4_client_surface *surface;

    TRACE( "%p %p %p %p\n", hwnd, instance, handle, client );

    if (!(surface = client_surface_create( sizeof(*surface), &ps4drv_client_surface_funcs, hwnd )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (instance->p_vkCreateHeadlessSurfaceEXT( instance->host.instance, &info, NULL /* allocator */, handle ))
    {
        ERR( "Failed to create a headless surface\n" );
        client_surface_release( &surface->client );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    surface->host = (unsigned long long)*handle;
    pthread_mutex_lock( &surfaces_lock );
    list_add_tail( &surfaces, &surface->entry );
    pthread_mutex_unlock( &surfaces_lock );

    TRACE( "Created surface 0x%s, client %s\n", wine_dbgstr_longlong( *handle ), debugstr_client_surface( &surface->client ) );
    *client = &surface->client;
    return VK_SUCCESS;
}

static VkBool32 PS4DRV_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device,
                                                                 uint32_t index )
{
    return VK_TRUE;
}

static void PS4DRV_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void PS4DRV_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs ps4drv_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = PS4DRV_vulkan_surface_create,
    .p_get_physical_device_presentation_support = PS4DRV_get_physical_device_presentation_support,
    .p_map_instance_extensions = PS4DRV_map_instance_extensions,
    .p_map_device_extensions = PS4DRV_map_device_extensions,
};

UINT PS4DRV_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    const char *log = getenv( "WINEPS4_VULKAN_LOG" );

    if (!log || strcmp( log, "0" )) ps4kit_set_log( vulkan_log );
    ps4kit_set_window_present( present_frame );
    *driver_funcs = &ps4drv_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
