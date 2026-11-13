// Clean-room Wine window-mode WSI for the PS4 RADV build.
//
// Design inputs:
// - WinePS4's unchanged port/ps4_unixlibs.c exports the kit's
//   ps4kit_vk_get_instance_proc_addr() as libvulkan.so.1.
// - WinePS4's patches/wine-ps4.patch registers ps4kit_window_present() and
//   uses the VkSurfaceKHR as the HWND lookup key.
// - The public SSBU WSI demonstrates the Mesa ICD dispatch symbols and
//   linear host-visible RADV image allocation. Its VideoOut flip path is
//   intentionally NOT used here.
//
// This implementation owns the Vulkan surface/swapchain dispatch. It creates
// linear CPU-readable images and synchronously sends completed pixels to the
// Wine desktop callback. It does not call VideoOut.
//
// SPDX-License-Identifier: MIT

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ps4_kit_vulkan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <vector>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_common_GetDeviceProcAddr(VkDevice device, const char *name);

namespace ps4_kit::wsi {
namespace {

constexpr uint32_t kSurfaceMagic = 0x50534b53; // "PSKS"
constexpr uint32_t kSwapchainMagic = 0x50534b57; // "PSKW"
constexpr uint32_t kMaxExtent = 8192;

struct Surface {
    uint32_t magic = kSurfaceMagic;
};

struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapping = nullptr;
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout layout{};
};

struct Swapchain {
    uint32_t magic = kSwapchainMagic;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool bgra = false;
    uint32_t next_image = 0;
    std::vector<SwapchainImage> images;
};

std::mutex g_lock;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_acquire_queue = VK_NULL_HANDLE;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<Swapchain>> g_swapchains;

template <typename T>
T instance_function(VkInstance instance, const char *name)
{
    return reinterpret_cast<T>(vk_icdGetInstanceProcAddr(instance, name));
}

template <typename T>
T device_function(VkDevice device, const char *name)
{
    return reinterpret_cast<T>(vk_common_GetDeviceProcAddr(device, name));
}

template <typename Handle>
Handle handle_from_pointer(void *pointer)
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<Handle>(pointer);
    else
        return static_cast<Handle>(reinterpret_cast<uintptr_t>(pointer));
}

template <typename Handle>
void *pointer_from_handle(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<void *>(handle);
    else
        return reinterpret_cast<void *>(static_cast<uintptr_t>(handle));
}

template <typename Handle>
unsigned long long handle_value(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle));
    else
        return static_cast<unsigned long long>(handle);
}

bool valid_surface(VkSurfaceKHR surface)
{
    const auto *object = static_cast<const Surface *>(pointer_from_handle(surface));
    return object && object->magic == kSurfaceMagic;
}

bool is_supported_format(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

VkResult enumerate_instance_extensions(const char *layer, uint32_t *count,
                                       VkExtensionProperties *properties)
{
    auto real = instance_function<PFN_vkEnumerateInstanceExtensionProperties>(
        VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return real(layer, count, properties);

    uint32_t real_count = 0;
    VkResult result = real(nullptr, &real_count, nullptr);
    if (result != VK_SUCCESS) return result;
    std::vector<VkExtensionProperties> list(real_count);
    result = real(nullptr, &real_count, list.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
    list.resize(real_count);

    constexpr const char *extra[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME
    };
    for (const char *name : extra) {
        const auto found = std::find_if(list.begin(), list.end(), [name](const auto &entry) {
            return std::strcmp(entry.extensionName, name) == 0;
        });
        if (found == list.end()) {
            VkExtensionProperties entry{};
            std::strncpy(entry.extensionName, name, sizeof(entry.extensionName) - 1);
            entry.specVersion = std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0 ? 25 : 1;
            list.push_back(entry);
        }
    }

    if (!properties) {
        *count = static_cast<uint32_t>(list.size());
        return VK_SUCCESS;
    }
    const uint32_t capacity = *count;
    const uint32_t copied = std::min<uint32_t>(capacity, static_cast<uint32_t>(list.size()));
    std::copy_n(list.begin(), copied, properties);
    *count = copied;
    return copied < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult enumerate_device_extensions(VkPhysicalDevice physical_device, const char *layer,
                                     uint32_t *count, VkExtensionProperties *properties)
{
    auto real = instance_function<PFN_vkEnumerateDeviceExtensionProperties>(
        g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return real(physical_device, layer, count, properties);

    uint32_t real_count = 0;
    VkResult result = real(physical_device, nullptr, &real_count, nullptr);
    if (result != VK_SUCCESS) return result;
    std::vector<VkExtensionProperties> list(real_count);
    result = real(physical_device, nullptr, &real_count, list.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
    list.resize(real_count);

    const auto found = std::find_if(list.begin(), list.end(), [](const auto &entry) {
        return std::strcmp(entry.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    });
    if (found == list.end()) {
        VkExtensionProperties entry{};
        std::strncpy(entry.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                     sizeof(entry.extensionName) - 1);
        entry.specVersion = 70;
        list.push_back(entry);
    }

    if (!properties) {
        *count = static_cast<uint32_t>(list.size());
        return VK_SUCCESS;
    }
    const uint32_t capacity = *count;
    const uint32_t copied = std::min<uint32_t>(capacity, static_cast<uint32_t>(list.size()));
    std::copy_n(list.begin(), copied, properties);
    *count = copied;
    return copied < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_create_instance(const VkInstanceCreateInfo *info,
                                        const VkAllocationCallbacks *allocator,
                                        VkInstance *instance)
{
    auto real = instance_function<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<const char *> enabled;
    enabled.reserve(info->enabledExtensionCount);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char *name = info->ppEnabledExtensionNames[i];
        if (std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0 ||
            std::strcmp(name, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0)
            continue;
        enabled.push_back(name);
    }

    VkInstanceCreateInfo copy = *info;
    copy.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    copy.ppEnabledExtensionNames = enabled.data();
    VkResult result = real(&copy, allocator, instance);
    if (result == VK_SUCCESS) {
        std::lock_guard guard(g_lock);
        g_instance = *instance;
    }
    return result;
}

VkResult VKAPI_CALL wsi_create_device(VkPhysicalDevice physical_device,
                                      const VkDeviceCreateInfo *info,
                                      const VkAllocationCallbacks *allocator,
                                      VkDevice *device)
{
    auto real = instance_function<PFN_vkCreateDevice>(g_instance, "vkCreateDevice");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<const char *> enabled;
    enabled.reserve(info->enabledExtensionCount);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char *name = info->ppEnabledExtensionNames[i];
        if (std::strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            continue;
        enabled.push_back(name);
    }

    VkDeviceCreateInfo copy = *info;
    copy.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    copy.ppEnabledExtensionNames = enabled.data();
    VkResult result = real(physical_device, &copy, allocator, device);
    if (result != VK_SUCCESS) return result;

    uint32_t family_count = 0;
    auto get_queues = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!get_queues) {
        auto destroy = device_function<PFN_vkDestroyDevice>(*device, "vkDestroyDevice");
        if (destroy) destroy(*device, allocator);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    get_queues(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_queues(physical_device, &family_count, families.data());

    uint32_t graphics_family = UINT32_MAX;
    for (uint32_t requested = 0; requested < info->queueCreateInfoCount; ++requested) {
        const uint32_t candidate = info->pQueueCreateInfos[requested].queueFamilyIndex;
        if (candidate < family_count && info->pQueueCreateInfos[requested].queueCount &&
            families[candidate].queueCount && (families[candidate].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics_family = candidate;
            break;
        }
    }
    if (graphics_family == UINT32_MAX) {
        auto destroy = device_function<PFN_vkDestroyDevice>(*device, "vkDestroyDevice");
        if (destroy) destroy(*device, allocator);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    auto get_queue = device_function<PFN_vkGetDeviceQueue>(*device, "vkGetDeviceQueue");
    if (!get_queue) {
        auto destroy = device_function<PFN_vkDestroyDevice>(*device, "vkDestroyDevice");
        if (destroy) destroy(*device, allocator);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkQueue acquire_queue = VK_NULL_HANDLE;
    get_queue(*device, graphics_family, 0, &acquire_queue);

    {
        std::lock_guard guard(g_lock);
        g_physical_device = physical_device;
        g_device = *device;
        g_acquire_queue = acquire_queue;
    }
    return VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_create_headless_surface(VkInstance, const VkHeadlessSurfaceCreateInfoEXT *,
                                                const VkAllocationCallbacks *, VkSurfaceKHR *surface)
{
    if (!surface) return VK_ERROR_INITIALIZATION_FAILED;
    *surface = handle_from_pointer<VkSurfaceKHR>(new (std::nothrow) Surface());
    return *surface ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void VKAPI_CALL wsi_destroy_surface(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks *)
{
    if (!valid_surface(surface)) return;
    delete static_cast<Surface *>(pointer_from_handle(surface));
}

VkResult VKAPI_CALL wsi_surface_support(VkPhysicalDevice physical_device, uint32_t family,
                                        VkSurfaceKHR surface, VkBool32 *supported)
{
    if (!supported || !valid_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
    uint32_t count = 0;
    auto get_queues = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!get_queues) return VK_ERROR_INITIALIZATION_FAILED;
    get_queues(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    get_queues(physical_device, &count, families.data());
    *supported = family < count && families[family].queueCount &&
                 (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

VkImageUsageFlags supported_linear_usage(VkPhysicalDevice physical_device, VkFormat format)
{
    auto get_properties = instance_function<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        g_instance, "vkGetPhysicalDeviceImageFormatProperties");
    if (!get_properties) return 0;
    constexpr VkImageUsageFlags candidates[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT
    };
    VkImageUsageFlags supported = 0;
    for (VkImageUsageFlags usage : candidates) {
        VkImageFormatProperties properties{};
        if (get_properties(physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
                           usage, 0, &properties) == VK_SUCCESS)
            supported |= usage;
    }
    return supported;
}

VkResult VKAPI_CALL wsi_surface_capabilities(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                             VkSurfaceCapabilitiesKHR *caps)
{
    if (!caps || !valid_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
    *caps = {};
    caps->minImageCount = 2;
    caps->maxImageCount = 3;
    // Headless surfaces have no native size. The application chooses the window size.
    caps->currentExtent = {UINT32_MAX, UINT32_MAX};
    caps->minImageExtent = {1, 1};
    caps->maxImageExtent = {kMaxExtent, kMaxExtent};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkImageUsageFlags rgba = supported_linear_usage(physical_device, VK_FORMAT_R8G8B8A8_UNORM);
    const VkImageUsageFlags bgra = supported_linear_usage(physical_device, VK_FORMAT_B8G8R8A8_UNORM);
    const bool have_rgba = (rgba & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    const bool have_bgra = (bgra & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    caps->supportedUsageFlags = have_rgba && have_bgra ? (rgba & bgra) : (have_rgba ? rgba : bgra);
    return VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_surface_capabilities2(VkPhysicalDevice physical_device,
                                              const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                              VkSurfaceCapabilities2KHR *caps)
{
    if (!info || !caps) return VK_ERROR_INITIALIZATION_FAILED;
    return wsi_surface_capabilities(physical_device, info->surface, &caps->surfaceCapabilities);
}

VkResult VKAPI_CALL wsi_surface_formats2(VkPhysicalDevice physical_device,
                                             const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                             uint32_t *count, VkSurfaceFormat2KHR *formats)
{
    if (!info || !count) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t available = 0;
    VkResult result = wsi_surface_formats(physical_device, info->surface, &available, nullptr);
    if (result != VK_SUCCESS) return result;
    if (!formats) {
        *count = available;
        return VK_SUCCESS;
    }
    const uint32_t capacity = *count;
    std::vector<VkSurfaceFormatKHR> base(available);
    result = wsi_surface_formats(physical_device, info->surface, &available, base.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
    const uint32_t copied = std::min<uint32_t>(capacity, available);
    for (uint32_t i = 0; i < copied; ++i) formats[i].surfaceFormat = base[i];
    *count = copied;
    return copied < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_surface_formats(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                        uint32_t *count, VkSurfaceFormatKHR *formats)
{
    if (!count || !valid_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
    std::vector<VkSurfaceFormatKHR> supported;
    if (supported_linear_usage(physical_device, VK_FORMAT_R8G8B8A8_UNORM) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        supported.push_back({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    if (supported_linear_usage(physical_device, VK_FORMAT_B8G8R8A8_UNORM) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        supported.push_back({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    if (!formats) {
        *count = static_cast<uint32_t>(supported.size());
        return VK_SUCCESS;
    }
    const uint32_t copied = std::min<uint32_t>(*count, static_cast<uint32_t>(supported.size()));
    std::copy_n(supported.begin(), copied, formats);
    *count = copied;
    return copied < supported.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_present_modes(VkPhysicalDevice, VkSurfaceKHR surface,
                                      uint32_t *count, VkPresentModeKHR *modes)
{
    if (!count || !valid_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!modes) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0) return VK_INCOMPLETE;
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}

uint32_t find_host_coherent_memory_type(VkPhysicalDevice physical_device, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties properties{};
    auto get_properties = instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
        g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!get_properties) return UINT32_MAX;
    get_properties(physical_device, &properties);
    const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

void destroy_swapchain_images(Swapchain &swapchain)
{
    auto unmap = device_function<PFN_vkUnmapMemory>(swapchain.device, "vkUnmapMemory");
    auto destroy_image = device_function<PFN_vkDestroyImage>(swapchain.device, "vkDestroyImage");
    auto free_memory = device_function<PFN_vkFreeMemory>(swapchain.device, "vkFreeMemory");
    for (auto &image : swapchain.images) {
        if (image.mapping && unmap) unmap(swapchain.device, image.memory);
        if (image.image && destroy_image) destroy_image(swapchain.device, image.image, nullptr);
        if (image.memory && free_memory) free_memory(swapchain.device, image.memory, nullptr);
        image = {};
    }
    swapchain.images.clear();
}

VkResult create_swapchain_image(Swapchain &swapchain, VkPhysicalDevice physical_device,
                                const VkSwapchainCreateInfoKHR &swapchain_info, SwapchainImage &out)
{
    auto create_image = device_function<PFN_vkCreateImage>(swapchain.device, "vkCreateImage");
    auto get_requirements = device_function<PFN_vkGetImageMemoryRequirements>(
        swapchain.device, "vkGetImageMemoryRequirements");
    auto allocate = device_function<PFN_vkAllocateMemory>(swapchain.device, "vkAllocateMemory");
    auto bind = device_function<PFN_vkBindImageMemory>(swapchain.device, "vkBindImageMemory");
    auto map = device_function<PFN_vkMapMemory>(swapchain.device, "vkMapMemory");
    auto get_layout = device_function<PFN_vkGetImageSubresourceLayout>(
        swapchain.device, "vkGetImageSubresourceLayout");
    if (!create_image || !get_requirements || !allocate || !bind || !map || !get_layout)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = swapchain.format;
    image_info.extent = {swapchain.width, swapchain.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = swapchain_info.imageUsage;
    image_info.sharingMode = swapchain_info.imageSharingMode;
    image_info.queueFamilyIndexCount = swapchain_info.queueFamilyIndexCount;
    image_info.pQueueFamilyIndices = swapchain_info.pQueueFamilyIndices;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = create_image(swapchain.device, &image_info, nullptr, &out.image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements requirements{};
    get_requirements(swapchain.device, out.image, &requirements);
    const uint32_t memory_type = find_host_coherent_memory_type(physical_device, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = allocate(swapchain.device, &allocation, nullptr, &out.memory);
    if (result != VK_SUCCESS) return result;

    result = bind(swapchain.device, out.image, out.memory, 0);
    if (result != VK_SUCCESS) return result;

    result = map(swapchain.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapping);
    if (result != VK_SUCCESS) return result;

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    get_layout(swapchain.device, out.image, &subresource, &layout);
    out.layout = layout;
    out.allocation_size = requirements.size;
    if (layout.offset >= requirements.size || layout.rowPitch < swapchain.width * 4u ||
        layout.size > requirements.size - layout.offset)
        return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                         const VkAllocationCallbacks *, VkSwapchainKHR *handle)
{
    if (!info || !handle || !valid_surface(info->surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (info->flags || info->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!is_supported_format(info->imageFormat) || info->imageArrayLayers != 1 ||
        info->imageExtent.width == 0 || info->imageExtent.height == 0 ||
        info->imageExtent.width > kMaxExtent || info->imageExtent.height > kMaxExtent)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    constexpr VkImageUsageFlags supported_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!info->imageUsage || (info->imageUsage & ~supported_usage) ||
        (info->imageUsage & ~supported_linear_usage(g_physical_device, info->imageFormat)))
        return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;
    if (info->minImageCount < 2 || info->minImageCount > 3 ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        info->presentMode != VK_PRESENT_MODE_FIFO_KHR ||
        (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount < 2))
        return VK_ERROR_INITIALIZATION_FAILED;

    auto swapchain = std::make_unique<Swapchain>();
    swapchain->device = device;
    swapchain->surface = info->surface;
    swapchain->format = info->imageFormat;
    swapchain->width = info->imageExtent.width;
    swapchain->height = info->imageExtent.height;
    swapchain->bgra = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM;
    swapchain->images.resize(std::clamp<uint32_t>(info->minImageCount, 2, 3));

    for (auto &image : swapchain->images) {
        VkResult result = create_swapchain_image(*swapchain, g_physical_device, *info, image);
        if (result != VK_SUCCESS) {
            destroy_swapchain_images(*swapchain);
            return result;
        }
    }
    const auto &first = swapchain->images.front();
    if (first.layout.rowPitch > UINT32_MAX) {
        destroy_swapchain_images(*swapchain);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSwapchainKHR key = handle_from_pointer<VkSwapchainKHR>(swapchain.get());
    {
        std::lock_guard guard(g_lock);
        g_swapchains.emplace(key, std::move(swapchain));
    }
    *handle = key;
    return VK_SUCCESS;
}

void VKAPI_CALL wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR handle,
                                      const VkAllocationCallbacks *)
{
    std::unique_ptr<Swapchain> swapchain;
    {
        std::lock_guard guard(g_lock);
        auto found = g_swapchains.find(handle);
        if (found == g_swapchains.end()) return;
        swapchain = std::move(found->second);
        g_swapchains.erase(found);
    }
    auto wait_idle = device_function<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle");
    if (wait_idle) wait_idle(device);
    destroy_swapchain_images(*swapchain);
    swapchain->magic = 0;
}

VkResult VKAPI_CALL wsi_get_swapchain_images(VkDevice, VkSwapchainKHR handle,
                                            uint32_t *count, VkImage *images)
{
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard guard(g_lock);
    auto found = g_swapchains.find(handle);
    if (found == g_swapchains.end()) return VK_ERROR_OUT_OF_DATE_KHR;
    const auto &stored = found->second->images;
    if (!images) {
        *count = static_cast<uint32_t>(stored.size());
        return VK_SUCCESS;
    }
    const uint32_t copied = std::min<uint32_t>(*count, static_cast<uint32_t>(stored.size()));
    for (uint32_t i = 0; i < copied; ++i) images[i] = stored[i].image;
    *count = copied;
    return copied < stored.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_acquire_next_image(VkDevice device, VkSwapchainKHR handle, uint64_t,
                                           VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    if (!index) return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock guard(g_lock);
    auto found = g_swapchains.find(handle);
    if (found == g_swapchains.end()) return VK_ERROR_OUT_OF_DATE_KHR;
    Swapchain &swapchain = *found->second;
    const uint32_t selected = swapchain.next_image++ % static_cast<uint32_t>(swapchain.images.size());
    VkQueue queue = g_acquire_queue;
    guard.unlock();

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (semaphore) {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &semaphore;
    }
    auto queue_submit = device_function<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
    if (!queue_submit || !queue) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = queue_submit(queue, 1, &submit, fence);
    if (result == VK_SUCCESS) *index = selected;
    return result;
}

VkResult VKAPI_CALL wsi_acquire_next_image2(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                            uint32_t *index)
{
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    return wsi_acquire_next_image(device, info->swapchain, info->timeout,
                                  info->semaphore, info->fence, index);
}

VkResult VKAPI_CALL wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (!info || !info->swapchainCount) return VK_ERROR_INITIALIZATION_FAILED;

    // An empty submit consumes the caller's binary wait semaphores and signals a fence only
    // after all rendering queued before presentation has completed. The callback therefore
    // never observes an image that the GPU is still writing.
    VkDevice device = g_device;
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto create_fence = device_function<PFN_vkCreateFence>(device, "vkCreateFence");
    auto destroy_fence = device_function<PFN_vkDestroyFence>(device, "vkDestroyFence");
    auto queue_submit = device_function<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
    auto wait_fences = device_function<PFN_vkWaitForFences>(device, "vkWaitForFences");
    if (!create_fence || !destroy_fence || !queue_submit || !wait_fences)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult result = create_fence(device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) return result;

    VkSubmitInfo wait_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    wait_submit.waitSemaphoreCount = info->waitSemaphoreCount;
    wait_submit.pWaitSemaphores = info->pWaitSemaphores;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    std::vector<VkPipelineStageFlags> wait_stages(info->waitSemaphoreCount, wait_stage);
    wait_submit.pWaitDstStageMask = wait_stages.data();
    result = queue_submit(queue, 1, &wait_submit, fence);
    if (result == VK_SUCCESS)
        result = wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    destroy_fence(device, fence, nullptr);
    if (result != VK_SUCCESS) return result;

    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        const VkSwapchainKHR handle = info->pSwapchains[i];
        const uint32_t image_index = info->pImageIndices[i];
        const void *pixels = nullptr;
        unsigned width = 0, height = 0, row_bytes = 0;
        int bgra = 0;
        {
            std::lock_guard guard(g_lock);
            auto found = g_swapchains.find(handle);
            if (found == g_swapchains.end() || image_index >= found->second->images.size()) {
                if (info->pResults) info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
                overall = VK_ERROR_OUT_OF_DATE_KHR;
                continue;
            }
            const Swapchain &swapchain = *found->second;
            const SwapchainImage &image = swapchain.images[image_index];
            pixels = static_cast<const uint8_t *>(image.mapping) + image.layout.offset;
            width = swapchain.width;
            height = swapchain.height;
            row_bytes = static_cast<uint32_t>(image.layout.rowPitch);
            bgra = swapchain.bgra ? 1 : 0;
            // The callback is synchronous. Keep the swapchain lock until it returns so
            // destruction cannot unmap the image while the desktop is consuming the pixels.
            ps4kit_window_present(handle_value(swapchain.surface), pixels, width, height, row_bytes, bgra);
        }
        if (info->pResults) info->pResults[i] = VK_SUCCESS;
    }
    return overall;
}

PFN_vkVoidFunction wsi_intercept(const char *name)
{
#define WSI_FN(vk_name, fn) \
    if (std::strcmp(name, "vk" #vk_name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
    WSI_FN(EnumerateInstanceExtensionProperties, enumerate_instance_extensions);
    WSI_FN(CreateInstance, wsi_create_instance);
    WSI_FN(EnumerateDeviceExtensionProperties, enumerate_device_extensions);
    WSI_FN(CreateDevice, wsi_create_device);
    WSI_FN(CreateHeadlessSurfaceEXT, wsi_create_headless_surface);
    WSI_FN(DestroySurfaceKHR, wsi_destroy_surface);
    WSI_FN(GetPhysicalDeviceSurfaceSupportKHR, wsi_surface_support);
    WSI_FN(GetPhysicalDeviceSurfaceCapabilitiesKHR, wsi_surface_capabilities);
    WSI_FN(GetPhysicalDeviceSurfaceCapabilities2KHR, wsi_surface_capabilities2);
    WSI_FN(GetPhysicalDeviceSurfaceFormats2KHR, wsi_surface_formats2);
    WSI_FN(GetPhysicalDeviceSurfaceFormatsKHR, wsi_surface_formats);
    WSI_FN(GetPhysicalDeviceSurfacePresentModesKHR, wsi_present_modes);
    WSI_FN(CreateSwapchainKHR, wsi_create_swapchain);
    WSI_FN(DestroySwapchainKHR, wsi_destroy_swapchain);
    WSI_FN(GetSwapchainImagesKHR, wsi_get_swapchain_images);
    WSI_FN(AcquireNextImageKHR, wsi_acquire_next_image);
    WSI_FN(AcquireNextImage2KHR, wsi_acquire_next_image2);
    WSI_FN(QueuePresentKHR, wsi_queue_present);
#undef WSI_FN
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL wsi_get_device_proc_addr(VkDevice device, const char *name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&wsi_get_device_proc_addr);
    if (auto intercepted = wsi_intercept(name)) return intercepted;
    return vk_common_GetDeviceProcAddr(device, name);
}

PFN_vkVoidFunction VKAPI_CALL wsi_get_instance_proc_addr(VkInstance instance, const char *name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&wsi_get_instance_proc_addr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&wsi_get_device_proc_addr);
    if (auto intercepted = wsi_intercept(name)) return intercepted;
    return vk_icdGetInstanceProcAddr(instance, name);
}

} // namespace
} // namespace ps4_kit::wsi

extern "C" void *ps4kit_vk_get_instance_proc_addr(void)
{
    return reinterpret_cast<void *>(ps4_kit::wsi::wsi_get_instance_proc_addr);
}
