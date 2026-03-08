/*
 * drm.h – Direct Rendering Manager Headers for RISC OS Phoenix
 * Defines DRM structures and functions for GPU/display integration
 * Supports KMS (Kernel Mode Setting) for modesetting
 * Author: R Andrews Grok 4 – 4 Feb 2026
 */

#ifndef DRM_H
#define DRM_H

#include <stdint.h>

#define DRM_MAX_MODES    32

typedef struct drm_mode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh;  // Hz
    uint32_t clock;    // MHz
    uint32_t hsync_start, hsync_end, htotal;
    uint32_t vsync_start, vsync_end, vtotal;
} drm_mode_t;

typedef struct drm_device {
    int fd;                 // DRM file descriptor
    void *native_window;    // For EGL/Vulkan surface
    drm_mode_t modes[DRM_MAX_MODES];
    int num_modes;
    drm_mode_t current_mode;
    // ... other fields (connectors, encoders, CRTCs)
} drm_device_t;

/* DRM functions */
drm_device_t *drm_open(const char *device);  // e.g., "/dev/dri/card0"
int drm_create_surface(drm_device_t *dev, drm_mode_t *mode, VkSurfaceKHR *surface);
int drm_set_mode(drm_device_t *dev, drm_mode_t *mode);
void drm_close(drm_device_t *dev);

#endif /* DRM_H */