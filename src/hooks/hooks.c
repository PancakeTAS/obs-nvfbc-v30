#define _GNU_SOURCE
#include "hooks.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <link.h>
#include <vulkan/vulkan.h>

#define GLX_NAME "libGLX.so.0"
#define VK_NAME "libvulkan.so.1"
#define GLX_SENTINEL_HANDLE ((void*) 1)
#define VK_SENTINEL_HANDLE ((void*) 2)

// stubs
char* glGetStringStub() { return "hewwo :3"; }
void* glStub() { return NULL; }
int glXStub() { return 1; }

/**
 * Hook glXGetProcAddress to nullify NvFBCs OpenGL calls
 *
 * \author
 *   0xNULLderef
 *
 * \param name
 *   The name of the function to get
 */
void* glXGetProcAddress_hook(const char* name) {
    if (!strcmp(name, "glGetString"))
        return glGetStringStub;
    return glStub;
}

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_real; //!< Real vkGetInstanceProcAddr function
PFN_vkCreateDevice vkCreateDevice_real; //!< Real vkCreateDevice function
PFN_vkAllocateMemory vkAllocateMemory_real; //!< Real vkAllocateMemory function

NvFBCCustomState gstate; //!< Global state
int gstate_index = 0; //!< Index of the fd inside the global state

VkResult vkCreateDevice_hook(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    VkResult res = vkCreateDevice_real(physicalDevice, pCreateInfo, pAllocator, pDevice);
    gstate.device = *pDevice;
    return res;
}

VkResult vkAllocateMemory_hook(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    VkResult res = vkAllocateMemory_real(device, pAllocateInfo, pAllocator, pMemory);
    if (pAllocateInfo->allocationSize > 10000) {
        gstate.memory[gstate_index] = *pMemory;
        gstate.size[gstate_index] = pAllocateInfo->allocationSize;
        gstate_index++;

        if (gstate_index == 2)
            gstate_index = 0;
    }
    return res;
}

/**
 * Hook vkGetInstanceProcAddr to hack with Vulkan
 *
 * \author
 *   Pancake
 *
 */
void* vkGetInstanceProcAddr_hook(VkInstance instance, const char* name) {
    gstate.instance = instance;

    if (!strcmp(name, "vkCreateDevice")) {
        vkCreateDevice_real = (PFN_vkCreateDevice) vkGetInstanceProcAddr_real(instance, name);
        return (void*) vkCreateDevice_hook;
    } else if (!strcmp(name, "vkAllocateMemory")) {
        vkAllocateMemory_real = (PFN_vkAllocateMemory) vkGetInstanceProcAddr_real(instance, name);
        return (void*) vkAllocateMemory_hook;
    }

    return vkGetInstanceProcAddr_real(instance, name);
}

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);

typedef void*(*dlopen_t)(const char* file, int mode);
typedef void*(*dlsym_t)(void* handle, const char* name);
typedef int(*dlclose_t)(void* handle);

dlopen_t dlopen_real; //!< Original dlopen function
dlsym_t dlsym_real; //!< Original dlsym function
dlclose_t dlclose_real; //!< Original dlclose function
void* glxhandle_real; //!< Handle to the real libGLX.so.0
void* vkhandle_real; //!< Handle to the real libvulkan.so.1

/**
 * Initialize the hooking mechanism
 *
 * \author
 *   0xNULLderef
 */
void dl_hook_init() {
    if (dlopen_real)
        return;

    dlopen_real = (dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    dlsym_real = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    dlclose_real = (dlclose_t)dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.2.5");
}

/**
 * Hook dlopen to intercept libGLX.so.0
 *
 * \author
 *   0xNULLderef
 *
 * \param file
 *   The file to open
 * \param mode
 *   The mode to open the file in
 */
void* dlopen(const char* file, int mode) {
    dl_hook_init();

    // check if call is from nvfbc
    void* ret = __builtin_extract_return_addr(__builtin_return_address(0));
    Dl_info info;
    dladdr(ret, &info);
    if (!strstr(info.dli_fname, "libnvidia-fbc")) {
        return dlopen_real(file, mode);
    }
    printf("\ndlopen on %s\n", file);

    if (file && !strcmp(GLX_NAME, file)) {
        glxhandle_real = dlopen_real(file, mode);
        return GLX_SENTINEL_HANDLE;
    } else if (file && !strcmp(VK_NAME, file)) {
        vkhandle_real = dlopen_real(file, mode);
        return VK_SENTINEL_HANDLE;
    } else {
        return dlopen_real(file, mode);
    }
}

/**
 * Hook dlsym to intercept glXGetProcAddress
 *
 * \author
 *   0xNULLderef
 *
 * \param handle
 *   The handle to search in
 * \param name
 *   The name of the function to get
 */
void* dlsym(void* handle, const char* name) {
    dl_hook_init();
    if (handle == GLX_SENTINEL_HANDLE) {
        printf("\ndlsym on %p %s\n", handle, name);
        if (!strcmp(name, "glXGetProcAddress"))
            return glXGetProcAddress_hook;
        else if (!strcmp(name, "glXCreateNewContext") || !strcmp(name, "glXMakeCurrent") || !strcmp(name, "glXDestroyContext"))
            return glXStub;
        return dlsym_real(glxhandle_real, name);
    } else if (handle == VK_SENTINEL_HANDLE) {
        printf("\ndlsym on %p %s\n", handle, name);
        if (!strcmp(name, "vkGetInstanceProcAddr")) {
            vkGetInstanceProcAddr_real = (PFN_vkGetInstanceProcAddr) dlsym_real(vkhandle_real, name);
            return vkGetInstanceProcAddr_hook;
        }
        return dlsym_real(vkhandle_real, name);
    } else {
        return dlsym_real(handle, name);
    }
}

/**
 * Hook dlclose to intercept libGLX.so.0
 *
 * \author
 *   0xNULLderef
 *
 * \param handle
 *   The handle to close
 */
int dlclose(void* handle) {
    dl_hook_init();
    if (handle == GLX_SENTINEL_HANDLE) {
        return dlclose_real(glxhandle_real);
    } else if (handle == VK_SENTINEL_HANDLE) {
        return dlclose_real(vkhandle_real);
    } else {
        return dlclose_real(handle);
    }
}