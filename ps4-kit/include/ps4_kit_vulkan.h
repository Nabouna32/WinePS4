#ifndef PS4_KIT_VULKAN_H
#define PS4_KIT_VULKAN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps4kit_window_present_fn)(unsigned long long surface, const void *pixels,
                                         unsigned width, unsigned height,
                                         unsigned row_bytes, int bgra);
typedef void (*ps4kit_vulkan_log_fn)(const char *line);

/* Called by Wine's PS4 Vulkan driver during PS4DRV_VulkanInit(). */
void ps4kit_set_window_present(ps4kit_window_present_fn hook);
void ps4kit_set_log(ps4kit_vulkan_log_fn log);

/* Called by the kit's Vulkan WSI after a presented image is ready for the desktop. */
void ps4kit_window_present(unsigned long long surface, const void *pixels,
                           unsigned width, unsigned height, unsigned row_bytes, int bgra);
void ps4kit_vulkan_log(const char *line);

#ifdef __cplusplus
}
#endif

#endif
