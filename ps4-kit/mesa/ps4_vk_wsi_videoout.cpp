// Clean-room native VideoOut WSI for the PS4 RADV build.
//
// Separate from the Wine window-desktop WSI. This profile creates linear
// host-visible RADV images, aligns and registers their buffers with VideoOut,
// and queues flips through the GNM winsys. It uses off-screen acquire waits and
// filters binary queue semaphores while preserving timeline semaphores.
//
// Behavioral references: Resonance, Crema, M33, SSBU and SMW. Their WSI files
// are not copied; this implementation is derived from the MIT window WSI in
// this repository and independently implements the native VideoOut backend.
//
// SPDX-License-Identifier: MIT

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);

namespace ps4_kit::videoout_wsi {
namespace {

constexpr uint32_t kSurfaceMagic = 0x50534b53; // "PSKS"
constexpr uint32_t kSwapchainMagic = 0x50534b57; // "PSKW"
constexpr uint32_t kScreenWidth = 1920;
constexpr uint32_t kScreenHeight = 1080;
constexpr uint32_t kMaxImages = 4;
constexpr VkDeviceSize kFramebufferAlign = 2u << 20;
constexpr int kVideoOutBanks = 4;

struct Surface {
    uint32_t magic = kSurfaceMagic;
};

struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapping = nullptr;
    VkDeviceSize allocation_size = 0;
    VkDeviceSize bind_offset = 0;
    VkSubresourceLayout layout{};
    int64_t last_flip_arg = 0;
};

struct Swapchain {
    uint32_t magic = kSwapchainMagic;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t next_image = 0;
    uint32_t flip_mode = ORBIS_VIDEO_OUT_FLIP_VSYNC;
    uint32_t pitch = 0;
    int buffer_set = -1;
    int first_buffer = 0;
    std::vector<SwapchainImage> images;
};

std::mutex g_lock;
std::mutex g_video_lock;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_acquire_queue = VK_NULL_HANDLE;
int g_video = -1;
OrbisKernelEqueue g_flip_queue = 0;
uint64_t g_flips = 0;
bool g_videoout_banks[kVideoOutBanks]{};
std::vector<std::unique_ptr<Swapchain>> g_retired;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<Swapchain>> g_swapchains;
std::mutex g_timeline_lock;
std::unordered_set<VkSemaphore> g_timelines;

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

bool valid_surface(VkSurfaceKHR surface)
{
    const auto *object = static_cast<const Surface *>(pointer_from_handle(surface));
    return object && object->magic == kSurfaceMagic;
}

bool is_supported_format(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM;
}

bool open_video_out()
{
    std::lock_guard guard(g_video_lock);
    if (g_video >= 0)
        return true;

    // SystemService is an internal module; the OpenOrbis sample loads it
    // explicitly before calling sceSystemServiceHideSplashScreen().
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);

    g_video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (g_video < 0)
        return false;

    int result = sceKernelCreateEqueue(&g_flip_queue, "ps4-kit videoout flip");
    if (result >= 0)
        result = sceVideoOutAddFlipEvent(g_flip_queue, g_video, nullptr);
    if (result < 0)
        g_flip_queue = 0; // fall back to short polling in wait_off_screen()
    return true;
}

bool wait_off_screen(int64_t required_flip_arg)
{
    if (required_flip_arg <= 0)
        return true;

    for (unsigned int tries = 0; tries < 40u; ++tries) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(g_video, &status) < 0)
            return false;
        if (status.flipArg >= required_flip_arg)
            return true;

        if (!g_flip_queue) {
            sceKernelUsleep(1000);
        } else {
            OrbisKernelEvent event{};
            int got = 0;
            OrbisKernelUseconds timeout = 5000;
            sceKernelWaitEqueue(g_flip_queue, &event, 1, &got, &timeout);
        }
    }
    return false;
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
                                                   VkSurfaceCapabilitiesKHR *capabilities)
{
    if (!capabilities || !valid_surface(surface))
        return VK_ERROR_SURFACE_LOST_KHR;
    *capabilities = {};
    capabilities->minImageCount = 2;
    capabilities->maxImageCount = 3;
    capabilities->currentExtent = {kScreenWidth, kScreenHeight};
    capabilities->minImageExtent = capabilities->currentExtent;
    capabilities->maxImageExtent = capabilities->currentExtent;
    capabilities->maxImageArrayLayers = 1;
    capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    capabilities->supportedUsageFlags = supported_linear_usage(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM) &
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
         VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
    if (!info || !count)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t available = 0;
    VkResult result = wsi_surface_formats(physical_device, info->surface, &available, nullptr);
    if (result != VK_SUCCESS)
        return result;
    if (!formats) {
        *count = available;
        return VK_SUCCESS;
    }
    const uint32_t capacity = *count;
    std::vector<VkSurfaceFormatKHR> base(available);
    result = wsi_surface_formats(physical_device, info->surface, &available, base.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;
    const uint32_t copied = std::min<uint32_t>(capacity, available);
    for (uint32_t i = 0; i < copied; ++i)
        formats[i].surfaceFormat = base[i];
    *count = copied;
    return copied < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_surface_formats(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                        uint32_t *count, VkSurfaceFormatKHR *formats)
{
    if (!count || !valid_surface(surface))
        return VK_ERROR_SURFACE_LOST_KHR;
    const VkImageUsageFlags supported = supported_linear_usage(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM);
    if ((supported & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!formats) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0)
        return VK_INCOMPLETE;
    formats[0] = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count = 1;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_present_modes(VkPhysicalDevice, VkSurfaceKHR surface,
                                      uint32_t *count, VkPresentModeKHR *modes)
{
    if (!count || !valid_surface(surface))
        return VK_ERROR_SURFACE_LOST_KHR;
    constexpr VkPresentModeKHR supported[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR
    };
    if (!modes) {
        *count = static_cast<uint32_t>(std::size(supported));
        return VK_SUCCESS;
    }
    const uint32_t copied = std::min<uint32_t>(*count, static_cast<uint32_t>(std::size(supported)));
    std::copy_n(std::begin(supported), copied, modes);
    *count = copied;
    return copied < std::size(supported) ? VK_INCOMPLETE : VK_SUCCESS;
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
        if (image.mapping && unmap)
            unmap(swapchain.device, image.memory);
        if (image.image && destroy_image)
            destroy_image(swapchain.device, image.image, nullptr);
        if (image.memory && free_memory)
            free_memory(swapchain.device, image.memory, nullptr);
        image = {};
    }
    swapchain.images.clear();
}

void release_retired()
{
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        Swapchain &swapchain = **it;
        if (swapchain.buffer_set >= 0 &&
            sceVideoOutUnregisterBuffers(g_video, swapchain.buffer_set) < 0) {
            ++it;
            continue;
        }
        if (swapchain.buffer_set >= 0) {
            g_videoout_banks[swapchain.first_buffer / static_cast<int>(kMaxImages)] = false;
            swapchain.buffer_set = -1;
        }
        destroy_swapchain_images(swapchain);
        swapchain.magic = 0;
        it = g_retired.erase(it);
    }
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
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {kScreenWidth, kScreenHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = swapchain_info.imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = swapchain_info.imageSharingMode;
    image_info.queueFamilyIndexCount = swapchain_info.queueFamilyIndexCount;
    image_info.pQueueFamilyIndices = swapchain_info.pQueueFamilyIndices;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = create_image(swapchain.device, &image_info, nullptr, &out.image);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements{};
    get_requirements(swapchain.device, out.image, &requirements);
    if (!requirements.alignment)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkDeviceSize alignment = std::max<VkDeviceSize>(kFramebufferAlign, requirements.alignment);
    if ((alignment & (alignment - 1u)) != 0 ||
        requirements.size > UINT64_MAX - alignment)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size + alignment;
    allocation.memoryTypeIndex = find_host_coherent_memory_type(physical_device, requirements.memoryTypeBits);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    result = allocate(swapchain.device, &allocation, nullptr, &out.memory);
    if (result != VK_SUCCESS)
        return result;

    result = map(swapchain.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapping);
    if (result != VK_SUCCESS)
        return result;

    const uintptr_t base = reinterpret_cast<uintptr_t>(out.mapping);
    const uintptr_t aligned = (base + alignment - 1u) & ~(static_cast<uintptr_t>(alignment) - 1u);
    out.bind_offset = static_cast<VkDeviceSize>(aligned - base);
    out.allocation_size = allocation.allocationSize;
    if (out.bind_offset > out.allocation_size ||
        requirements.size > out.allocation_size - out.bind_offset)
        return VK_ERROR_INITIALIZATION_FAILED;

    result = bind(swapchain.device, out.image, out.memory, out.bind_offset);
    if (result != VK_SUCCESS)
        return result;

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    get_layout(swapchain.device, out.image, &subresource, &out.layout);

    const uintptr_t video_address = base + out.bind_offset + out.layout.offset;
    if ((video_address & (kFramebufferAlign - 1u)) != 0 ||
        out.layout.rowPitch % 4u != 0 ||
        out.layout.rowPitch / 4u < kScreenWidth ||
        out.layout.rowPitch / 4u > UINT32_MAX ||
        out.layout.offset >= requirements.size ||
        out.layout.size > requirements.size - out.layout.offset)
        return VK_ERROR_INITIALIZATION_FAILED;

    return VK_SUCCESS;
}

VkResult VKAPI_CALL wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                         const VkAllocationCallbacks *, VkSwapchainKHR *handle)
{
    if (!info || !handle || !valid_surface(info->surface))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (info->flags || info->pNext)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!is_supported_format(info->imageFormat) || info->imageArrayLayers != 1 ||
        info->imageExtent.width != kScreenWidth || info->imageExtent.height != kScreenHeight ||
        info->minImageCount < 2 || info->minImageCount > 3 ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if (info->presentMode != VK_PRESENT_MODE_FIFO_KHR &&
        info->presentMode != VK_PRESENT_MODE_MAILBOX_KHR &&
        info->presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    constexpr VkImageUsageFlags supported_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags linear_usage = supported_linear_usage(g_physical_device, info->imageFormat);
    const VkImageUsageFlags required_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((linear_usage & required_usage) != required_usage ||
        !info->imageUsage || (info->imageUsage & ~supported_usage) ||
        (info->imageUsage & ~linear_usage))
        return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;

    if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!open_video_out())
        return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard guard(g_lock);
    release_retired();

    int bank = -1;
    for (int i = 0; i < kVideoOutBanks; ++i) {
        if (!g_videoout_banks[i]) {
            bank = i;
            break;
        }
    }
    if (bank < 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    auto swapchain = std::make_unique<Swapchain>();
    swapchain->device = device;
    swapchain->surface = info->surface;
    swapchain->format = info->imageFormat;
    swapchain->width = kScreenWidth;
    swapchain->height = kScreenHeight;
    swapchain->flip_mode = info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR
                               ? ORBIS_VIDEO_OUT_FLIP_HSYNC
                               : ORBIS_VIDEO_OUT_FLIP_VSYNC;
    swapchain->first_buffer = bank * static_cast<int>(kMaxImages);
    swapchain->images.resize(std::clamp<uint32_t>(info->minImageCount, 2u, 3u));

    void *addresses[kMaxImages]{};
    uint32_t pitch = 0;
    for (size_t i = 0; i < swapchain->images.size(); ++i) {
        VkResult result = create_swapchain_image(*swapchain, g_physical_device, *info,
                                                 swapchain->images[i]);
        if (result != VK_SUCCESS) {
            destroy_swapchain_images(*swapchain);
            return result;
        }
        const auto &image = swapchain->images[i];
        addresses[i] = static_cast<uint8_t *>(image.mapping) + image.bind_offset + image.layout.offset;
        const uint32_t image_pitch = static_cast<uint32_t>(image.layout.rowPitch / 4u);
        if (i && image_pitch != pitch) {
            destroy_swapchain_images(*swapchain);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        pitch = image_pitch;
    }
    swapchain->pitch = pitch;

    OrbisVideoOutBufferAttribute attribute{};
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0,
                                  kScreenWidth, kScreenHeight, pitch);
    swapchain->buffer_set = sceVideoOutRegisterBuffers(
        g_video, swapchain->first_buffer, addresses,
        static_cast<int>(swapchain->images.size()), &attribute);
    if (swapchain->buffer_set < 0) {
        destroy_swapchain_images(*swapchain);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    g_videoout_banks[bank] = true;
    VkSwapchainKHR key = handle_from_pointer<VkSwapchainKHR>(swapchain.get());
    g_swapchains.emplace(key, std::move(swapchain));
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
        if (found == g_swapchains.end())
            return;
        swapchain = std::move(found->second);
        g_swapchains.erase(found);
    }

    auto wait_idle = device_function<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle");
    if (wait_idle)
        wait_idle(device);

    {
        std::lock_guard guard(g_lock);
        if (swapchain->buffer_set >= 0 &&
            sceVideoOutUnregisterBuffers(g_video, swapchain->buffer_set) < 0) {
            g_retired.push_back(std::move(swapchain));
            return;
        }
        if (swapchain->buffer_set >= 0) {
            g_videoout_banks[swapchain->first_buffer / static_cast<int>(kMaxImages)] = false;
            swapchain->buffer_set = -1;
        }
    }

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
    (void)semaphore; // VideoOut WSI does not signal binary acquire semaphores.
    if (!index)
        return VK_ERROR_INITIALIZATION_FAILED;

    {
        std::lock_guard guard(g_lock);
        auto found = g_swapchains.find(handle);
        if (found == g_swapchains.end())
            return VK_ERROR_OUT_OF_DATE_KHR;
        Swapchain &swapchain = *found->second;
        const uint32_t candidate = swapchain.next_image %
                                   static_cast<uint32_t>(swapchain.images.size());
        if (!wait_off_screen(swapchain.images[candidate].last_flip_arg))
            return VK_TIMEOUT;
        *index = candidate;
        swapchain.next_image = (candidate + 1u) %
                               static_cast<uint32_t>(swapchain.images.size());
    }

    if (fence) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        auto queue_submit = device_function<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
        if (!queue_submit || !g_acquire_queue)
            return VK_ERROR_INITIALIZATION_FAILED;
        return queue_submit(g_acquire_queue, 1, &submit, fence);
    }
    return VK_SUCCESS;
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
    if (!info || !info->swapchainCount || !info->pSwapchains || !info->pImageIndices)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        VkResult item_result = VK_SUCCESS;
        {
            std::lock_guard guard(g_lock);
            auto found = g_swapchains.find(info->pSwapchains[i]);
            if (found == g_swapchains.end() ||
                info->pImageIndices[i] >= found->second->images.size()) {
                item_result = VK_ERROR_OUT_OF_DATE_KHR;
            } else {
                Swapchain &swapchain = *found->second;
                SwapchainImage &presented_image = swapchain.images[info->pImageIndices[i]];
                const unsigned buffer = static_cast<unsigned>(swapchain.first_buffer) +
                                        info->pImageIndices[i];
                const int64_t arg = static_cast<int64_t>(g_flips + 1u);
                int result = ac_ps4_submit_flip(g_video, buffer, swapchain.flip_mode, arg);
                const bool gpu_flip = result == 0;
                if (!gpu_flip) {
                    auto wait_idle = device_function<PFN_vkQueueWaitIdle>(swapchain.device,
                                                                          "vkQueueWaitIdle");
                    if (!wait_idle) {
                        item_result = VK_ERROR_INITIALIZATION_FAILED;
                    } else {
                        const VkResult idle_result = wait_idle(queue);
                        if (idle_result != VK_SUCCESS) {
                            item_result = idle_result;
                        } else {
                            result = sceVideoOutSubmitFlip(g_video, static_cast<int>(buffer),
                                                           swapchain.flip_mode, arg);
                        }
                    }
                }

                if (item_result == VK_SUCCESS && result < 0) {
                    item_result = VK_ERROR_SURFACE_LOST_KHR;
                } else if (item_result == VK_SUCCESS) {
                    presented_image.last_flip_arg = arg;
                    const uint64_t submitted = ++g_flips;
                    if (submitted == 1u)
                        sceSystemServiceHideSplashScreen();
                    release_retired();
                }
            }
        }
        if (info->pResults)
            info->pResults[i] = item_result;
        if (item_result != VK_SUCCESS)
            overall = item_result;
    }
    return overall;
}

VkResult VKAPI_CALL wsi_create_semaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkSemaphore *semaphore)
{
    auto real = device_function<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = real(device, info, allocator, semaphore);
    if (result != VK_SUCCESS)
        return result;

    for (auto p = static_cast<const VkBaseInStructure *>(info->pNext); p; p = static_cast<const VkBaseInStructure *>(p->pNext)) {
        if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
            const auto *type_info = reinterpret_cast<const VkSemaphoreTypeCreateInfo *>(p);
            if (type_info->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
                std::lock_guard lock(g_timeline_lock);
                g_timelines.insert(*semaphore);
            }
        }
    }
    return result;
}

void VKAPI_CALL wsi_destroy_semaphore(VkDevice device, VkSemaphore semaphore,
                                      const VkAllocationCallbacks *allocator)
{
    {
        std::unique_lock lock(g_timeline_lock);
        g_timelines.erase(semaphore);
    }
    auto real = device_function<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore");
    if (real)
        real(device, semaphore, allocator);
}

bool is_timeline_semaphore(VkSemaphore semaphore)
{
    std::lock_guard lock(g_timeline_lock);
    return g_timelines.count(semaphore) != 0;
}

VkResult VKAPI_CALL wsi_queue_submit(VkQueue queue, uint32_t count,
                                     const VkSubmitInfo *submits, VkFence fence)
{
    auto real = device_function<PFN_vkQueueSubmit>(g_device, "vkQueueSubmit");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!count)
        return real(queue, count, submits, fence);

    std::vector<VkSubmitInfo> copy(submits, submits + count);
    std::vector<std::vector<VkSemaphore>> wait_semaphores(count), signal_semaphores(count);
    std::vector<std::vector<VkPipelineStageFlags>> wait_stages(count);
    std::vector<std::vector<uint64_t>> wait_values(count), signal_values(count);
    std::vector<VkTimelineSemaphoreSubmitInfo> timeline_copies(count);

    for (uint32_t i = 0; i < count; ++i) {
        const VkTimelineSemaphoreSubmitInfo *timeline = nullptr;
        const auto *head = static_cast<const VkBaseInStructure *>(submits[i].pNext);
        for (auto p = head; p; p = static_cast<const VkBaseInStructure *>(p->pNext)) {
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo *>(p);
                break;
            }
        }
        if (timeline && head != reinterpret_cast<const VkBaseInStructure *>(timeline))
            return VK_ERROR_FEATURE_NOT_PRESENT;

        for (uint32_t j = 0; j < submits[i].waitSemaphoreCount; ++j) {
            VkSemaphore semaphore = submits[i].pWaitSemaphores[j];
            if (!is_timeline_semaphore(semaphore))
                continue;
            wait_semaphores[i].push_back(semaphore);
            wait_stages[i].push_back(submits[i].pWaitDstStageMask[j]);
            wait_values[i].push_back(timeline && j < timeline->waitSemaphoreValueCount
                                         ? timeline->pWaitSemaphoreValues[j] : 0);
        }
        for (uint32_t j = 0; j < submits[i].signalSemaphoreCount; ++j) {
            VkSemaphore semaphore = submits[i].pSignalSemaphores[j];
            if (!is_timeline_semaphore(semaphore))
                continue;
            signal_semaphores[i].push_back(semaphore);
            signal_values[i].push_back(timeline && j < timeline->signalSemaphoreValueCount
                                           ? timeline->pSignalSemaphoreValues[j] : 0);
        }

        copy[i].waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores[i].size());
        copy[i].pWaitSemaphores = wait_semaphores[i].data();
        copy[i].pWaitDstStageMask = wait_stages[i].data();
        copy[i].signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores[i].size());
        copy[i].pSignalSemaphores = signal_semaphores[i].data();

        if (timeline || !wait_semaphores[i].empty() || !signal_semaphores[i].empty()) {
            VkTimelineSemaphoreSubmitInfo &filtered = timeline_copies[i];
            filtered = timeline ? *timeline
                                : VkTimelineSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            filtered.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values[i].size());
            filtered.pWaitSemaphoreValues = wait_values[i].data();
            filtered.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values[i].size());
            filtered.pSignalSemaphoreValues = signal_values[i].data();
            filtered.pNext = timeline ? timeline->pNext : submits[i].pNext;
            copy[i].pNext = &filtered;
        }
    }
    return real(queue, count, copy.data(), fence);
}

VkResult VKAPI_CALL wsi_queue_submit2(VkQueue queue, uint32_t count,
                                      const VkSubmitInfo2 *submits, VkFence fence)
{
    auto real = device_function<PFN_vkQueueSubmit2>(g_device, "vkQueueSubmit2");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!count)
        return real(queue, count, submits, fence);

    std::vector<VkSubmitInfo2> copy(submits, submits + count);
    std::vector<std::vector<VkSemaphoreSubmitInfo>> wait_infos(count), signal_infos(count);
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < submits[i].waitSemaphoreInfoCount; ++j)
            if (is_timeline_semaphore(submits[i].pWaitSemaphoreInfos[j].semaphore))
                wait_infos[i].push_back(submits[i].pWaitSemaphoreInfos[j]);
        for (uint32_t j = 0; j < submits[i].signalSemaphoreInfoCount; ++j)
            if (is_timeline_semaphore(submits[i].pSignalSemaphoreInfos[j].semaphore))
                signal_infos[i].push_back(submits[i].pSignalSemaphoreInfos[j]);
        copy[i].waitSemaphoreInfoCount = static_cast<uint32_t>(wait_infos[i].size());
        copy[i].pWaitSemaphoreInfos = wait_infos[i].data();
        copy[i].signalSemaphoreInfoCount = static_cast<uint32_t>(signal_infos[i].size());
        copy[i].pSignalSemaphoreInfos = signal_infos[i].data();
    }
    return real(queue, count, copy.data(), fence);
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
    WSI_FN(CreateSemaphore, wsi_create_semaphore);
    WSI_FN(DestroySemaphore, wsi_destroy_semaphore);
    WSI_FN(QueueSubmit, wsi_queue_submit);
    WSI_FN(QueueSubmit2, wsi_queue_submit2);
    WSI_FN(QueueSubmit2KHR, wsi_queue_submit2);
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
} // namespace ps4_kit::videoout_wsi

extern "C" void *ps4kit_vk_get_instance_proc_addr(void)
{
    return reinterpret_cast<void *>(ps4_kit::videoout_wsi::wsi_get_instance_proc_addr);
}
