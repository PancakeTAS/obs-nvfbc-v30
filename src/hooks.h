#pragma once

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <NvFBC.h>

typedef struct {
    uint8_t pad1[0x8];
    VkDeviceMemory memory;
    uint64_t size;
    int interop_fd;
} NvFBCVkSurface; //!< NvFBC Vulkan surface

typedef struct {
    uint8_t pad1[0x32];
    VkInstance instance;
    uint8_t pad2[0x8];
    VkDevice device;
    uint8_t pad3[0x238];
    NvFBCVkSurface surfaces[2];
} NvFBCState; //!< NvFBC state

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

/**
 * Get NvFBCState from session handle
 *
 * \author
 *   0xNULLderef
 *
 * \param session
 *   The NvFBC session handle
 * \return
 *   The NvFBC state
 */
NvFBCState* get_nvfb_state(NVFBC_SESSION_HANDLE session);
