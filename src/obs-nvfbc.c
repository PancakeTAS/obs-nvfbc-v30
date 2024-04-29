#define _GNU_SOURCE
#include <NvFBC.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <threads.h>

#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <obs/obs-module.h>

#include <link.h>

OBS_DECLARE_MODULE()

// vulkan forward declarations

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    uint64_t size;
    int interop_fd;
} nvfbc_vk_surface;

typedef struct {
    uint8_t pad1[0x32];
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    uint8_t pad2[0x220];
    VkDeviceMemory shared_area_memory;
    void* shared_area_memory_ptr;
    uint8_t pad3[0x8];
    nvfbc_vk_surface surfaces[3];
} nvfbc_state;

typedef nvfbc_state* (*FBCGetStateFromHandle_t)(const NVFBC_SESSION_HANDLE handle);
int get_module_base_callback(struct dl_phdr_info* info, size_t size, void* data_ptr);
uintptr_t get_module_base(const char* module_name);

// module

typedef struct {
    int tracking_type; //!< Tracking type (default, output, screen)
    char display_name[128]; //!< Display name (only if tracking_type is output)
    bool has_capture_area; //!< Has capture area
    int capture_x, capture_y, capture_width, capture_height; // < Capture area
    int frame_width, frame_height; //!< Frame size
    bool with_cursor; //!< Capture cursor
    bool push_model; //!< Push model
    int sampling_rate; //!< Sampling rate in ms (only if push_model is disabled)
    bool direct_capture; //!< Direct capture
} nvfbc_capture_t; //!< Capture data sent to subprocess for the NvFBC source

typedef struct {
    int width, height; //!< Resolution of the OBS source

    bool should_run; //!< Should NvFBC be running
    void* frame_ptr; //!< Frame buffer pointer

    obs_source_t* source; //!< OBS source
    gs_texture_t *textures[2]; //!< OBS texture
    int active_texture; //!< Active texture
    mtx_t lock; //!< Lock to prevent rendering while updating the frame buffer
    thrd_t thread; //!< Thread to capture frames
    nvfbc_capture_t capture_data; //!< Capture data sent to subprocess
} nvfbc_source_t; //!< Source data for the NvFBC source

static void start_source(void* data, obs_data_t* settings) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (source_data->should_run) return;

    mtx_lock(&source_data->lock);

    nvfbc_capture_t capture_data = {
        .frame_width = obs_data_get_int(settings, "width"),
        .frame_height = obs_data_get_int(settings, "height"),
        .with_cursor = obs_data_get_bool(settings, "with_cursor"),
        .sampling_rate = obs_data_get_int(settings, "sampling_rate"),
        .push_model = obs_data_get_int(settings, "sampling_rate") == 0
    };

    const char* tracking_type = obs_data_get_string(settings, "tracking_type");
    if (tracking_type[0] != '0' && tracking_type[0] != '2') {
        capture_data.tracking_type = 1;
        strncpy(capture_data.display_name, obs_data_get_string(settings, "tracking_type"), 127);
        strchr(capture_data.display_name, ':')[0] = '\0';
    } else {
        capture_data.tracking_type = tracking_type[0] - '0';
    }

    if (obs_data_get_bool(settings, "direct_capture")) {
        capture_data.direct_capture = true;
        capture_data.with_cursor = false;
        capture_data.push_model = true;
    }

    if (obs_data_get_bool(settings, "crop_area")) {
        capture_data.has_capture_area = true;
        capture_data.capture_x = obs_data_get_int(settings, "capture_x");
        capture_data.capture_y = obs_data_get_int(settings, "capture_y");
        capture_data.capture_width = obs_data_get_int(settings, "capture_width");
        capture_data.capture_height = obs_data_get_int(settings, "capture_height");
    }

    memcpy(&source_data->capture_data, &capture_data, sizeof(nvfbc_capture_t));
    source_data->frame_ptr = malloc(source_data->width * source_data->height * 4);

    source_data->should_run = true;

    mtx_unlock(&source_data->lock);
}

static void stop_source(void* data) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (!source_data->should_run) return;
    mtx_lock(&source_data->lock);
    source_data->should_run = false;
    mtx_unlock(&source_data->lock);
}

// rendering stuff

static void video_render(void* data, gs_effect_t* effect) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (!source_data->should_run) return;

    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, source_data->textures[source_data->active_texture]);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(source_data->textures[source_data->active_texture], 0, source_data->width, source_data->height);
}

static int capture_thread(void* data) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    nvfbc_capture_t* capture_data = &source_data->capture_data;


    // create nvfbc
    NVFBC_API_FUNCTION_LIST fbc = { .dwVersion = NVFBC_VERSION };
    NVFBCSTATUS status = NvFBCCreateInstance(&fbc);
    if (status) {
        fprintf(stderr, "Failed to create NvFBC instance\n");
        return 1;
    }

    NVFBC_SESSION_HANDLE handle;
    status = NvFBCCreateHandle(&handle, &(NVFBC_CREATE_HANDLE_PARAMS) {
        .dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER
    });
    if (status) {
        fprintf(stderr, "Failed to get NvFBC status: %s (%d)\n", NvFBCGetLastErrorStr(handle), status);
        return 1;
    }

    NVFBC_GET_STATUS_PARAMS status_params = {
        .dwVersion = NVFBC_GET_STATUS_PARAMS_VER
    };
    status = fbc.nvFBCGetStatus(handle, &status_params);
    if (status) {
        fprintf(stderr, "Failed to get NvFBC status: %s (%d)\n", NvFBCGetLastErrorStr(handle), status);
        return 1;
    }

    // grab vulkan
    uintptr_t nvfbc_base_addr = get_module_base("libnvidia-fbc.so.1");
    FBCGetStateFromHandle_t FBCGetStateFromHandle = (FBCGetStateFromHandle_t) (nvfbc_base_addr + 0xCB30);
    nvfbc_state* state = FBCGetStateFromHandle(handle);
    VkResult (*vkGetMemoryFdKHR)(VkDevice, const VkMemoryGetFdInfoKHR*, int*) = (void*) vkGetInstanceProcAddr(state->instance, "vkGetMemoryFdKHR");

    while (true) {
        if (!source_data->should_run) {
            thrd_sleep(&(struct timespec) { .tv_sec = 0, .tv_nsec = 10000000 }, NULL);
            continue;
        }

        // find display if using tracking type 1
        int dwOutputId = 0;
        if (capture_data->tracking_type == 1) {
            for (uint32_t i = 0; i < status_params.dwOutputNum; i++) {
                if (strncmp(capture_data->display_name, status_params.outputs[i].name, 127) == 0) {
                    dwOutputId = status_params.outputs[i].dwId;
                    break;
                }
            }
        }

        // create capture session
        NVFBC_CREATE_CAPTURE_SESSION_PARAMS session_params = {
            .dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
            .eCaptureType = NVFBC_CAPTURE_TO_GL,
            .bWithCursor = capture_data->with_cursor,
            .eTrackingType = capture_data->tracking_type,
            .frameSize = { .w = capture_data->frame_width, .h = capture_data->frame_height },
            .dwOutputId = dwOutputId,
            .dwSamplingRateMs = capture_data->sampling_rate,
            .bPushModel = capture_data->push_model,
            .bAllowDirectCapture = capture_data->direct_capture
        };
        if (capture_data->has_capture_area) {
            session_params.captureBox.x = capture_data->capture_x;
            session_params.captureBox.y = capture_data->capture_y;
            session_params.captureBox.w = capture_data->capture_width;
            session_params.captureBox.h = capture_data->capture_height;
        }
        status = fbc.nvFBCCreateCaptureSession(handle, &session_params);
        if (status) {
            fprintf(stderr, "Failed to create NvFBC capture session: %s (%d)\n", NvFBCGetLastErrorStr(handle), status);

            return 1;
        }

        // setup capture session
        NVFBC_TOGL_SETUP_PARAMS togl_params = {
            .dwVersion = NVFBC_TOGL_SETUP_PARAMS_VER,
            .eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA,
        };
        status = fbc.nvFBCToGLSetUp(handle, &togl_params);
        if (status) {
            fprintf(stderr, "Failed to setup NvFBC capture session: %s (%d)\n", NvFBCGetLastErrorStr(handle), status);
            return 1;
        }

        // get gl functions
        void* (*glCreateMemoryObjectsEXT)(GLsizei, GLuint*) = (void*) eglGetProcAddress("glCreateMemoryObjectsEXT");
        void* (*glMemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*) = (void*) eglGetProcAddress("glMemoryObjectParameterivEXT");
        void* (*glImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint) = (void*) eglGetProcAddress("glImportMemoryFdEXT");
        void* (*glTextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = (void*) eglGetProcAddress("glTextureStorageMem2DEXT");

        // fix fds
        obs_enter_graphics();
        for (int i = 0; i < 2; i++) {
            vkGetMemoryFdKHR(state->device, &(VkMemoryGetFdInfoKHR) {
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                .pNext = NULL,
                .memory = state->surfaces[i].memory,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
            }, &state->surfaces[i].interop_fd);

            // create obs texture
            GLuint mem_obj;
            glCreateMemoryObjectsEXT(1, &mem_obj);
            printf("glCreateMemoryObjectsEXT: %d\n", glGetError());

            GLint isDedicated = GL_TRUE;
            glMemoryObjectParameterivEXT(mem_obj, GL_DEDICATED_MEMORY_OBJECT_EXT, &isDedicated);
            printf("glMemoryObjectParameterivEXT: %d\n", glGetError());

            glImportMemoryFdEXT(mem_obj, state->surfaces[i].size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, state->surfaces[i].interop_fd);
            printf("glImportMemoryFdEXT: %d\n", glGetError());

            gs_texture_t* texture = gs_texture_create(capture_data->frame_width, capture_data->frame_height, GS_RGBA, 1, source_data->frame_ptr, 0);
            source_data->textures[i] = texture;
            GLuint gl_texture = *(GLuint*) gs_texture_get_obj(texture);

            glBindTexture(GL_TEXTURE_2D, gl_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTextureStorageMem2DEXT(gl_texture, 1, GL_RGBA8, capture_data->frame_width, capture_data->frame_height, mem_obj, 0);
            printf("glTextureStorageMem2DEXT: %d\n", glGetError());

            glBindTexture(GL_TEXTURE_2D, 0);

        }
        obs_leave_graphics();

        // capture loop
        NVFBC_TOGL_GRAB_FRAME_PARAMS togl_grab_params = {
            .dwVersion = NVFBC_TOGL_GRAB_FRAME_PARAMS_VER,
            .dwFlags = NVFBC_TOGL_GRAB_FLAGS_NOFLAGS
        };
        while (source_data->should_run) {
            status = fbc.nvFBCToGLGrabFrame(handle, &togl_grab_params);
            if (status) {
                fprintf(stderr, "Failed to grab NvFBC frame: %s (%d)\n", NvFBCGetLastErrorStr(handle), status);
                return 1;
            }

            printf("Frame grabbed: %d\n", togl_grab_params.dwTextureIndex);
            source_data->active_texture = togl_grab_params.dwTextureIndex;
        }

        // cleanup
        fbc.nvFBCDestroyCaptureSession(handle, &(NVFBC_DESTROY_CAPTURE_SESSION_PARAMS) { .dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER });
        free(source_data->frame_ptr);

    }

    return 0;
}

// obs configuration stuff

static bool on_direct_update(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    obs_property_set_visible(obs_properties_get(props, "with_cursor"), !obs_data_get_bool(settings, "direct_capture"));
    obs_property_set_visible(obs_properties_get(props, "sampling_rate"), !obs_data_get_bool(settings, "direct_capture"));
    return true;
}

static bool on_crop_update(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    obs_property_set_visible(obs_properties_get(props, "capture_area"), obs_data_get_bool(settings, "crop_area"));
    return true;
}

static bool on_reload(obs_properties_t* unused, obs_property_t *unused1, void *data) {
    stop_source(data);
    start_source(data, obs_source_get_settings(((nvfbc_source_t*) data)->source));
    return true;
}

static obs_properties_t* get_properties(void* unused) {
    obs_properties_t* props = obs_properties_create();

    // tracking type
    obs_property_t* prop = obs_properties_add_list(props, "tracking_type", "Tracking Type", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(prop, "Primary Screen", "0");
    obs_property_list_add_string(prop, "Entire X Screen", "2");

    xcb_connection_t* conn = xcb_connect(NULL, NULL);
    xcb_randr_get_monitors_reply_t* monitors = xcb_randr_get_monitors_reply(conn, xcb_randr_get_monitors(conn, xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root, 1), NULL);
    xcb_randr_monitor_info_iterator_t iter = xcb_randr_get_monitors_monitors_iterator(monitors);
    for (; iter.rem; xcb_randr_monitor_info_next(&iter)) {
        xcb_randr_monitor_info_t* monitor = iter.data;

        xcb_get_atom_name_reply_t* name = xcb_get_atom_name_reply(conn, xcb_get_atom_name(conn, monitor->name), NULL);
        char name_str[128];
        sprintf(name_str, "%.*s: %dx%d+%d+%d", name->name_len, xcb_get_atom_name_name(name), monitor->width, monitor->height, monitor->x, monitor->y);
        obs_property_list_add_string(prop, name_str, name_str);

        free(name);
    }

    xcb_disconnect(conn);
    free(monitors);

    prop = obs_properties_add_bool(props, "direct_capture", "Allow direct capture");
    obs_property_set_modified_callback(prop, on_direct_update);
    obs_properties_add_bool(props, "with_cursor", "Track Cursor");

    // capture area
    prop = obs_properties_add_bool(props, "crop_area", "Crop capture area");
    obs_property_set_modified_callback(prop, on_crop_update);
    obs_properties_t* crop_props = obs_properties_create();
    obs_properties_add_int(crop_props, "capture_x", "Capture X", 0, 4096, 2);
    obs_properties_add_int(crop_props, "capture_y", "Capture Y", 0, 4096, 2);
    obs_properties_add_int(crop_props, "capture_width", "Capture Width", 0, 4096, 2);
    obs_properties_add_int(crop_props, "capture_height", "Capture Height", 0, 4096, 2);
    obs_properties_add_group(props, "capture_area", "Capture Area", OBS_GROUP_NORMAL, crop_props);

    // frame size
    obs_properties_t* resize_props = obs_properties_create();
    obs_properties_add_int(resize_props, "width", "Frame Width", 0, 4096, 2);
    obs_properties_add_int(resize_props, "height", "Frame Height", 0, 4096, 2);
    obs_properties_add_int(resize_props, "sampling_rate", "Track Interval (ms)", 0, 1000, 1);
    obs_properties_add_group(props, "frame_size", "Frame Size", OBS_GROUP_NORMAL, resize_props);

    obs_properties_add_button(props, "settings", "Update settings", on_reload);

    return props;
}

static void get_defaults(obs_data_t* settings) {
    // tracking type
    obs_data_set_default_string(settings, "tracking_type", "0");

    // capture area
    obs_data_set_default_bool(settings, "crop_area", false);
    obs_data_set_default_int(settings, "capture_x", 0);
    obs_data_set_default_int(settings, "capture_y", 0);
    obs_data_set_default_int(settings, "capture_width", 1920);
    obs_data_set_default_int(settings, "capture_height", 1080);

    // frame size
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 1080);

    // misc capture options
    obs_data_set_default_bool(settings, "with_cursor", true);
    obs_data_set_default_int(settings, "sampling_rate", 16);
}

// obs module essentials

static void update(void* data, obs_data_t* settings) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    source_data->width = obs_data_get_int(settings, "width");
    source_data->height = obs_data_get_int(settings, "height");

    stop_source(data);
}

const char* get_name(void* unused) { return "NvFBC Source"; }
static uint32_t get_width(void* data) { return ((nvfbc_source_t*) data)->width; }
static uint32_t get_height(void* data) { return ((nvfbc_source_t*) data)->height; }

static void* create(obs_data_t* settings, obs_source_t* source) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) bzalloc(sizeof(nvfbc_source_t));
    source_data->source = source;
    mtx_init(&source_data->lock, mtx_plain);
    update(source_data, settings);

    thrd_create(&source_data->thread, capture_thread, source_data);
    thrd_detach(source_data->thread);
    start_source(source_data, settings);

    return source_data;
}

static void destroy(void* data) {
    stop_source(data);
    mtx_destroy(&((nvfbc_source_t*) data)->lock);
    bfree(data);
}

struct obs_source_info nvfbc_source = {
    .id = "nvfbc-source",
    .version = 1,
    .get_name = get_name,

    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,

    .create = create,
    .update = update,
    .destroy = destroy,
    .video_render = video_render,

    .get_properties = get_properties,
    .get_defaults = get_defaults,

    .get_width = get_width,
    .get_height = get_height,
};

bool obs_module_load(void) {
    obs_register_source(&nvfbc_source);
    return true;
}

// address stuff

struct get_module_base_data_t {
    const char* module_name;
    uintptr_t base_address;
};

int get_module_base_callback(struct dl_phdr_info* info, size_t size, void* data_ptr) {
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

uintptr_t get_module_base(const char* module_name) {
    struct get_module_base_data_t data = { .module_name = module_name, .base_address = 0 };
    dl_iterate_phdr(get_module_base_callback, (void*)&data);
    return data.base_address;
}

// hooking stuff

#define VK_NAME "libvulkan.so.1"
#define VK_SENTINEL_HANDLE ((void*) 1)
#define GLX_NAME "libGLX.so.0"
#define GLX_SENTINEL_HANDLE ((void*) 2)

VkResult vkCreateInstance_hook(VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    ((VkApplicationInfo*) pCreateInfo->pApplicationInfo)->apiVersion = VK_API_VERSION_1_3;
    return vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

VkResult vkCreateDevice_hook(VkPhysicalDevice physicalDevice, VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    static const char *deviceExtensions[] = {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME
    };

    pCreateInfo->ppEnabledExtensionNames = deviceExtensions;
    pCreateInfo->enabledExtensionCount = 6;
    return vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

VkResult vkGetPhysicalDeviceImageFormatProperties2_hook(VkPhysicalDevice physicalDevice, VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties) {
    pImageFormatInfo->usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
    return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

VkResult vkCreateImage_hook(VkDevice device, VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    VkImageCreateInfo newCreateInfo = *pCreateInfo;
    newCreateInfo.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
    return vkCreateImage(device, &newCreateInfo, pAllocator, pImage);
}

void* vkGetInstanceProcAddr_hook(VkInstance instance, const char* pName) {
    if (pName && !strcmp(pName, "vkCreateInstance"))
        return vkCreateInstance_hook;
    if (pName && !strcmp(pName, "vkCreateDevice"))
        return vkCreateDevice_hook;
    if (pName && !strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2"))
        return vkGetPhysicalDeviceImageFormatProperties2_hook;
    if (pName && !strcmp(pName, "vkCreateImage"))
        return vkCreateImage_hook;
    return vkGetInstanceProcAddr(instance, pName);
}

char* glGetString_hook() {
    return "hewwo :3";
}

void* gl_stub() {
    return NULL;
}

void* glXGetProcAddress_hook(const char* name) {
    if (!strcmp(name, "glGetString"))
        return glGetString_hook;
    return gl_stub;
}

int glX_stub() {
    return 1;
}

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);

typedef void*(*dlopen_t)(const char* file, int mode);
typedef void*(*dlsym_t)(void* handle, const char* name);
typedef int(*dlclose_t)(void* handle);

dlopen_t dlopen_real;
dlsym_t dlsym_real;
dlclose_t dlclose_real;
void* vkhandle_real;
void* glxhandle_real;

void dl_hook_init() {
    static bool is_inited = false;
    if(is_inited) return;

    is_inited = true;
    dlopen_real = (dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    dlsym_real = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    dlclose_real = (dlclose_t)dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.2.5");
}

void* dlopen(const char* file, int mode) {
    dl_hook_init();
    if(file && !strcmp(VK_NAME, file)) {
        vkhandle_real = dlopen_real(file, mode);
        return VK_SENTINEL_HANDLE;
    } else if (file && !strcmp(GLX_NAME, file)) {
        glxhandle_real = dlopen_real(file, mode);
        return GLX_SENTINEL_HANDLE;
    } else {
        return dlopen_real(file, mode);
    }
}

void* dlsym(void* handle, const char* name) {
    dl_hook_init();
    if(handle == VK_SENTINEL_HANDLE) {
        if (!strcmp(name, "vkGetInstanceProcAddr"))
            return vkGetInstanceProcAddr_hook;
        return dlsym_real(vkhandle_real, name);
    } else if (handle == GLX_SENTINEL_HANDLE) {
        if (!strcmp(name, "glXGetProcAddress"))
            return glXGetProcAddress_hook;
        else if (!strcmp(name, "glXCreateNewContext") || !strcmp(name, "glXMakeCurrent") || !strcmp(name, "glXDestroyContext"))
            return glX_stub;
        return dlsym_real(glxhandle_real, name);
    } else {
        return dlsym_real(handle, name);
    }
}

int dlclose(void* handle) {
    dl_hook_init();
    if(handle == VK_SENTINEL_HANDLE) {
        return dlclose_real(vkhandle_real);
    } else if (handle == GLX_SENTINEL_HANDLE) {
        return dlclose_real(glxhandle_real);
    } else {
        return dlclose_real(handle);
    }
}
