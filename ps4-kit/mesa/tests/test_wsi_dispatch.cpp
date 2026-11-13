#include <cassert>
#include <cstring>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ps4_kit_vulkan.h"

static void VKAPI_CALL fake_driver_function() {}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance, const char *name)
{
    if (std::strcmp(name, "vkDriverOnlyInstanceFunction") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&fake_driver_function);
    return nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_common_GetDeviceProcAddr(VkDevice, const char *name)
{
    if (std::strcmp(name, "vkDriverOnlyDeviceFunction") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&fake_driver_function);
    return nullptr;
}

extern "C" void ps4kit_window_present(unsigned long long, const void *,
                                       unsigned, unsigned, unsigned, int) {}
extern "C" void ps4kit_vulkan_log(const char *) {}

int main()
{
    auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        ps4kit_vk_get_instance_proc_addr());
    assert(get_instance);

    // Kit-owned entry points must be intercepted before RADV dispatch.
    const char *instance_entries[] = {
        "vkGetInstanceProcAddr",
        "vkGetDeviceProcAddr",
        "vkEnumerateInstanceExtensionProperties",
        "vkCreateInstance",
        "vkEnumerateDeviceExtensionProperties",
        "vkCreateDevice",
        "vkCreateHeadlessSurfaceEXT",
        "vkDestroySurfaceKHR",
        "vkGetPhysicalDeviceSurfaceSupportKHR",
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        "vkGetPhysicalDeviceSurfaceFormats2KHR",
        "vkGetPhysicalDeviceSurfacePresentModesKHR"
    };
    for (const char *name : instance_entries)
        assert(get_instance(VK_NULL_HANDLE, name));

    // Driver-owned instance functions must still reach RADV.
    assert(get_instance(VK_NULL_HANDLE, "vkDriverOnlyInstanceFunction") ==
           reinterpret_cast<PFN_vkVoidFunction>(&fake_driver_function));

    auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance(VK_NULL_HANDLE, "vkGetDeviceProcAddr"));
    assert(get_device);
    const char *device_entries[] = {
        "vkGetDeviceProcAddr",
        "vkCreateSwapchainKHR",
        "vkDestroySwapchainKHR",
        "vkGetSwapchainImagesKHR",
        "vkAcquireNextImageKHR",
        "vkAcquireNextImage2KHR",
        "vkQueuePresentKHR"
    };
    for (const char *name : device_entries)
        assert(get_device(VK_NULL_HANDLE, name));
    assert(get_device(VK_NULL_HANDLE, "vkDriverOnlyDeviceFunction") ==
           reinterpret_cast<PFN_vkVoidFunction>(&fake_driver_function));
    return 0;
}
