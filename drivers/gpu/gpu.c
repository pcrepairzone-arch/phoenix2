/*
 * gpu.c – GPU Acceleration Driver for RISC OS Phoenix
 * Primary: Vulkan (VideoCore VII / VI)
 * Fallback: OpenGL ES 2.0
 * Author: Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "vulkan.h"
#include "drm.h"
#include "wimp.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>

/* ==================== Vulkan Globals ==================== */
static VkInstance vk_instance;
static VkPhysicalDevice vk_gpu;
static VkDevice vk_device;
static VkQueue vk_queue;
static VkSwapchainKHR vk_swapchain;
static VkSurfaceKHR vk_surface;
static VkRenderPass render_pass;
static VkPipeline blit_pipeline;
static VkPipelineLayout pipeline_layout;
static VkCommandPool cmd_pool;
static VkCommandBuffer command_buffers[2];
static VkSemaphore image_acquired, render_finished;
static VkFramebuffer framebuffers[2];

/* ==================== GLES Fallback Globals ==================== */
static EGLDisplay egl_display;
static EGLSurface egl_surface;
static EGLContext egl_context;
static EGLConfig egl_config;

/* ==================== DRM / Display ==================== */
static drm_device_t *drm_dev;
static drm_mode_t current_mode = {3840, 2160, 120};

static int use_vulkan = 1;

/* ==================== Embedded SPIR-V Shaders ==================== */

/* Vertex Shader SPIR-V */
static const uint32_t vert_shader_spirv[] = {
    0x07230203,0x00010000,0x0008000A,0x0000001C,0x00000000,0x00020011,0x00000001,0x0006000B,
    0x00000001,0x4C534C47,0x6474732E,0x3035342E,0x00000000,0x0002000C,0x00000001,0x00000001,
    0x0006000B,0x00000001,0x4C534C47,0x746E2E6A,0x00000000,0x0007000B,0x00000001,0x4C534C47,
    0x2E303100,0x00000000,0x00000000,0x0003000E,0x00000000,0x00000000,0x0007000F,0x00000000,
    0x00000004,0x6E69616D,0x00000000,0x00000009,0x0000000C,0x00030003,0x00000002,0x000001C2,
    0x00090004,0x41535552,0x00000042,0x0000002A,0x00000000,0x00000000,0x00000000,0x00040005,
    0x00000004,0x6E69616D,0x00000000,0x00050005,0x00000009,0x74726576,0x00006F50,0x00000073,
    0x00050005,0x0000000C,0x74726576,0x00005655,0x00000000,0x00060006,0x0000000F,0x00000004,
    0x6C617266,0x746E656D,0x00000000,0x00030005,0x00000011,0x00000000,0x00060005,0x00000013,
    0x56553F4C,0x6863765F,0x6E6E6165,0x00306C65,0x00060006,0x00000013,0x00000000,0x505F6C67,
    0x65567265,0x78657472,0x00000000,0x00060006,0x00000013,0x00000001,0x505F6C67,0x746E696F,
    0x00000000,0x00050006,0x00000015,0x00000000,0x475F6C67,0x4C424F4C,0x00000053,0x00040005,
    0x0000001A,0x74726556,0x00000000,0x00050005,0x0000001B,0x74726576,0x00006F50,0x00000073,
    0x00030005,0x0000001C,0x00000000,0x00040047,0x00000009,0x0000001E,0x00000000,0x00040047,
    0x0000000C,0x0000001E,0x00000001,0x00040047,0x0000000F,0x0000001E,0x00000000,0x00040047,
    0x00000011,0x0000001E,0x00000000,0x00040047,0x00000015,0x00000022,0x00000000,0x00040047,
    0x00000015,0x00000021,0x00000000,0x00040047,0x0000001A,0x0000001E,0x00000000,0x00020013,
    0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,
    0x00000007,0x00000006,0x00000002,0x00040017,0x00000008,0x00000006,0x00000004,0x00040020,
    0x00000009,0x00000003,0x00000007,0x0004003B,0x00000009,0x0000000A,0x00000003,0x00040020,
    0x0000000B,0x00000001,0x00000007,0x0004003B,0x0000000B,0x0000000C,0x00000001,0x00040017,
    0x0000000D,0x00000006,0x00000003,0x00040020,0x0000000E,0x00000003,0x0000000D,0x0004003B,
    0x0000000E,0x0000000F,0x00000003,0x00040015,0x00000010,0x00000020,0x00000001,0x0004002B,
    0x00000010,0x00000012,0x00000000,0x00040020,0x00000013,0x00000003,0x00000007,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000005,0x000200F8,0x00000016,0x0004003D,0x00000007,
    0x00000017,0x0000000A,0x0004003D,0x00000007,0x00000018,0x0000000C,0x00050051,0x00000006,
    0x00000019,0x00000018,0x00000000,0x00050051,0x00000006,0x0000001A,0x00000018,0x00000001,
    0x00070050,0x00000008,0x0000001B,0x00000019,0x0000001A,0x00000006,0x00000006,0x0003003E,
    0x0000000F,0x0000001B,0x000100FD,0x00010038
};

/* Fragment Shader SPIR-V */
static const uint32_t frag_shader_spirv[] = {
    0x07230203,0x00010000,0x0008000A,0x0000001D,0x00000000,0x00020011,0x00000001,0x0006000B,
    0x00000001,0x4C534C47,0x6474732E,0x3035342E,0x00000000,0x0002000C,0x00000001,0x00000001,
    0x0006000B,0x00000001,0x4C534C47,0x746E2E6A,0x00000000,0x0007000B,0x00000001,0x4C534C47,
    0x2E303100,0x00000000,0x00000000,0x0003000E,0x00000000,0x00000000,0x0007000F,0x00000004,
    0x00000004,0x6E69616D,0x00000000,0x00000009,0x0000000C,0x00030010,0x00000004,0x00000007,
    0x00030003,0x00000002,0x000001C2,0x00090004,0x41535552,0x00000042,0x0000002A,0x00000000,
    0x00000000,0x00000000,0x00040005,0x00000004,0x6E69616D,0x00000000,0x00050005,0x00000009,
    0x74726576,0x00006F50,0x00000073,0x00050005,0x0000000C,0x74726576,0x00005655,0x00000000,
    0x00060006,0x0000000F,0x00000004,0x6C617266,0x746E656D,0x00000000,0x00030005,0x00000011,
    0x00000000,0x00060005,0x00000013,0x56553F4C,0x6863765F,0x6E6E6165,0x00306C65,0x00060006,
    0x00000013,0x00000000,0x505F6C67,0x65567265,0x78657472,0x00000000,0x00060006,0x00000013,
    0x00000001,0x505F6C67,0x746E696F,0x00000000,0x00050006,0x00000015,0x00000000,0x475F6C67,
    0x4C424F4C,0x00000053,0x00040005,0x0000001A,0x74726556,0x00000000,0x00050005,0x0000001B,
    0x74726576,0x00006F50,0x00000073,0x00030005,0x0000001C,0x00000000,0x00040047,0x00000009,
    0x0000001E,0x00000000,0x00040047,0x0000000C,0x0000001E,0x00000001,0x00040047,0x0000000F,
    0x0000001E,0x00000000,0x00040047,0x00000011,0x0000001E,0x00000000,0x00040047,0x00000015,
    0x00000022,0x00000000,0x00040047,0x00000015,0x00000021,0x00000000,0x00040047,0x0000001A,
    0x0000001E,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,0x00040017,0x00000008,
    0x00000006,0x00000004,0x00040020,0x00000009,0x00000001,0x00000007,0x0004003B,0x00000009,
    0x0000000A,0x00000001,0x00040020,0x0000000B,0x00000003,0x00000007,0x0004003B,0x0000000B,
    0x0000000C,0x00000003,0x00040017,0x0000000D,0x00000006,0x00000003,0x00040020,0x0000000E,
    0x00000003,0x0000000D,0x0004003B,0x0000000E,0x0000000F,0x00000003,0x00040015,0x00000010,
    0x00000020,0x00000001,0x0004002B,0x00000010,0x00000012,0x00000000,0x00040020,0x00000013,
    0x00000003,0x00000007,0x00050036,0x00000002,0x00000004,0x00000000,0x00000005,0x000200F8,
    0x00000016,0x0004003D,0x00000007,0x00000017,0x0000000A,0x0004003D,0x00000007,0x00000018,
    0x0000000C,0x00050051,0x00000006,0x00000019,0x00000018,0x00000000,0x00050051,0x00000006,
    0x0000001A,0x00000018,0x00000001,0x00070050,0x00000008,0x0000001B,0x00000019,0x0000001A,
    0x00000006,0x00000006,0x0003003E,0x0000000F,0x0000001B,0x000100FD,0x00010038
};

/* ==================== Vulkan Redraw ==================== */
void vulkan_redraw_window(window_t *win)
{
    if (!win || !win->texture) return;

    uint32_t image_index;
    vkAcquireNextImageKHR(vk_device, vk_swapchain, UINT64_MAX,
                          image_acquired, VK_NULL_HANDLE, &image_index);

    VkCommandBuffer cmd = command_buffers[image_index];

    vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    });

    VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffers[image_index],
        .renderArea = {{0, 0}, {current_mode.width, current_mode.height}},
        .clearValueCount = 1,
        .pClearValues = &clear
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipeline);

    gpu_bind_texture(cmd, win->texture);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_finished
    };

    vkQueueSubmit(vk_queue, 1, &submit, VK_NULL_HANDLE);

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished,
        .swapchainCount = 1,
        .pSwapchains = &vk_swapchain,
        .pImageIndices = &image_index
    };

    vkQueuePresentKHR(vk_queue, &present);
}

/* ==================== GLES Fallback Redraw ==================== */
void gles_redraw_window(window_t *win)
{
    if (!win || !win->texture) return;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, win->texture->gl_id);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    eglSwapBuffers(egl_display, egl_surface);
}

/* ==================== Unified Redraw Dispatcher ==================== */
void gpu_redraw_window(window_t *win)
{
    if (use_vulkan) {
        vulkan_redraw_window(win);
    } else {
        gles_redraw_window(win);
    }
}

/* ==================== Module Init ==================== */
_kernel_oserror *module_init(const char *arg, int podule)
{
    if (gpu_init() != 0) {
        debug_print("GPU acceleration disabled\n");
        return NULL;
    }

    wimp_set_redraw_callback(gpu_redraw_window);
    debug_print("GPU module loaded – hardware acceleration active\n");
    return NULL;
}