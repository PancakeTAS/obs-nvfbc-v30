#pragma once

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <NvFBC.h>

// TODO: remove unused stuff
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    uint64_t size;
    int interop_fd;
} NvFBCVkSurface; //!< NvFBC Vulkan surface

typedef struct {
    uint8_t pad1[0x32];
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    uint8_t pad2[0x220];
    VkDeviceMemory shared_area_memory;
    void* shared_area_memory_ptr;
    uint8_t pad3[0x8];
    NvFBCVkSurface surfaces[3];
} NvFBCState; //!< NvFBC state

/**
 * Get the base address of a module
 *
 * \param module_name
 *   The name of the module to get the base address of
 */
uintptr_t get_module_base(const char* module_name);

/**
 * Get NvFBCState from session handle
 *
 * \param session
 *   The NvFBC session handle
 * \return
 *   The NvFBC state
 */
NvFBCState* get_nvfb_state(NVFBC_SESSION_HANDLE session);
