#pragma once

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <NvFBC.h>

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkDeviceMemory memory[2];
    uint64_t size[2];
} NvFBCCustomState;

extern NvFBCCustomState gstate;

/**
 * Get the base address of a module
 *
 * \author
 *   0xNULLderef
 *
 * \param module_name
 *   The name of the module to get the base address of
 */
uintptr_t get_module_base(const char* module_name);
