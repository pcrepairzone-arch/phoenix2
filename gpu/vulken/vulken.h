/*
 * vulkan.h – Vulkan headers and embedded shaders
 */

#ifndef VULKAN_H
#define VULKAN_H

#include <vulkan/vulkan.h>

// Embedded SPIR-V shaders (Vertex + Fragment)
extern const uint32_t vert_shader_spirv[];
extern const uint32_t frag_shader_spirv[];

#endif /* VULKAN_H */