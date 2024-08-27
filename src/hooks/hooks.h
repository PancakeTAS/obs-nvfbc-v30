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
