#define _GNU_SOURCE
#include "hooks.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <link.h>

#define GLX_NAME "libGLX.so.0"
#define GLX_SENTINEL_HANDLE ((void*) 1)

// stubs
char* glGetStringStub() { return "hewwo :3"; }
void* glStub() { return NULL; }
int glXStub() { return 1; }

/**
 * Hook glXGetProcAddress to nullify NvFBCs OpenGL calls
 *
 * \param name
 *   The name of the function to get
 */
void* glXGetProcAddress_hook(const char* name) {
    if (!strcmp(name, "glGetString"))
        return glGetStringStub;
    return glStub;
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

/**
 * Initialize the hooking mechanism
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
 * \param file
 *   The file to open
 * \param mode
 *   The mode to open the file in
 */
void* dlopen(const char* file, int mode) {
    dl_hook_init();
    if (file && !strcmp(GLX_NAME, file)) {
        glxhandle_real = dlopen_real(file, mode);
        return GLX_SENTINEL_HANDLE;
    } else {
        return dlopen_real(file, mode);
    }
}

/**
 * Hook dlsym to intercept glXGetProcAddress
 *
 * \param handle
 *   The handle to search in
 * \param name
 *   The name of the function to get
 */
void* dlsym(void* handle, const char* name) {
    dl_hook_init();
    if (handle == GLX_SENTINEL_HANDLE) {
        if (!strcmp(name, "glXGetProcAddress"))
            return glXGetProcAddress_hook;
        else if (!strcmp(name, "glXCreateNewContext") || !strcmp(name, "glXMakeCurrent") || !strcmp(name, "glXDestroyContext"))
            return glXStub;
        return dlsym_real(glxhandle_real, name);
    } else {
        return dlsym_real(handle, name);
    }
}

/**
 * Hook dlclose to intercept libGLX.so.0
 *
 * \param handle
 *   The handle to close
 */
int dlclose(void* handle) {
    dl_hook_init();
    if (handle == GLX_SENTINEL_HANDLE) {
        return dlclose_real(glxhandle_real);
    } else {
        return dlclose_real(handle);
    }
}

struct get_module_base_data_t {
    const char* module_name;
    uintptr_t base_address;
};

static int get_module_base_callback(struct dl_phdr_info* info, size_t size, void* data_ptr) {
    struct get_module_base_data_t* data = (struct get_module_base_data_t*)data_ptr;

    if(info->dlpi_name) {
        char* base_name = strrchr(info->dlpi_name, '/');
        if(
            base_name &&
            !strcmp(base_name + 1, data->module_name)
        ) {
            data->base_address = info->dlpi_addr + info->dlpi_phdr[0].p_vaddr;
            return 1;
        }
    }

    return 0;
}

/**
 * Get the base address of a module
 *
 * \param module_name
 *   The name of the module to get the base address of
 *
 * \return
 *   The base address of the module
 */
uintptr_t get_module_base(const char* module_name) {
    struct get_module_base_data_t data = { .module_name = module_name, .base_address = 0 };
    dl_iterate_phdr(get_module_base_callback, (void*)&data);
    return data.base_address;
}

#define NVFBC_LIB "libnvidia-fbc.so.1"
#define NVFBC_OFFSET 0xCB30 // offset of FBCGetStateFromHandle in NvFBC

NvFBCState* get_nvfb_state(NVFBC_SESSION_HANDLE session) {
    uintptr_t base = get_module_base(NVFBC_LIB);
    NvFBCState* (*FBCGetStateFromHandle)(const NVFBC_SESSION_HANDLE) = (void*) (base + NVFBC_OFFSET);
    return FBCGetStateFromHandle(session);
}