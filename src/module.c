#include "hooks/hooks.h"
#include "source.h"

#include <vulkan/vulkan.h>
#include <obs/obs-module.h>
#include <EGL/egl.h>
#include <NvFBC.h>

OBS_DECLARE_MODULE()

typedef struct {
    NVFBC_SESSION_HANDLE session; //!< NvFBC session handle
    GLuint memory_objects[2]; //!< Memory objects
} nvfbc_user; //!< NvFBC user data

NVFBC_API_FUNCTION_LIST fbc = { .dwVersion = NVFBC_VERSION }; //!< NvFBC API function list

void* (*glCreateMemoryObjectsEXT)(GLsizei, GLuint*) = NULL; //!< glCreateMemoryObjectsEXT function pointer
void* (*glMemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*) = NULL; //!< glMemoryObjectParameterivEXT function pointer
void* (*glImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint) = NULL; //!< glImportMemoryFdEXT function pointer
void* (*glTextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = NULL; //!< glTextureStorageMem2DEXT function pointer
void* (*glDeleteMemoryObjectsEXT)(GLsizei, const GLuint*) = NULL; //!< glDeleteMemoryObjectsEXT function pointer

/**
 * Start capture
 *
 * \author
 *   PancakeTAS
 *
 * \param params
 *   Capture parameters
 */
void start_capture(capture_params* params) {
    blog(LOG_INFO, "Starting capture");

    nvfbc_user* user_data = (nvfbc_user*) calloc(1, sizeof(nvfbc_user));
    params->user_data = user_data;

    // create NvFBC session to grab status
    NVFBCSTATUS status = fbc.nvFBCCreateHandle(&user_data->session, &(NVFBC_CREATE_HANDLE_PARAMS) { .dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to create NvFBC session: %d", status);
        return;
    }

    // get NvFBC status
    NVFBC_GET_STATUS_PARAMS status_params = { .dwVersion = NVFBC_GET_STATUS_PARAMS_VER };
    status = fbc.nvFBCGetStatus(user_data->session, &status_params);
    if (status) {
        blog(LOG_ERROR, "Failed to get NvFBC status: %d", status);
        return;
    }

    // find output id
    int dwOutputId = -1;
    if (params->tracking_type == 1) {
        for (uint32_t i = 0; i < status_params.dwOutputNum; i++) {
            if (!strncmp(status_params.outputs[i].name, params->display_name, 127)) {
                dwOutputId = status_params.outputs[i].dwId;
                break;
            }
        }
    }

    // create NvFBC capture session
    status = fbc.nvFBCCreateCaptureSession(user_data->session, &(NVFBC_CREATE_CAPTURE_SESSION_PARAMS) {
        .dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
        .eCaptureType = NVFBC_CAPTURE_TO_GL,
        .bWithCursor = params->with_cursor,
        .eTrackingType = params->tracking_type,
        .frameSize = {
            .w = params->frame_width,
            .h = params->frame_height
        },
        .captureBox = {
            .x = params->has_capture_area ? params->capture_x : 0,
            .y = params->has_capture_area ? params->capture_y : 0,
            .w = params->has_capture_area ? params->capture_width : 0,
            .h = params->has_capture_area ? params->capture_height : 0
        },
        .dwOutputId = dwOutputId,
        .dwSamplingRateMs = params->sampling_rate,
        .bPushModel = params->push_model,
        .bAllowDirectCapture = params->direct_mode
    });
    if (status) {
        blog(LOG_ERROR, "Failed to create NvFBC capture session: %d", status);
        return;
    }

    // setup ToGL capture (it does absolutely nothing)
    status = fbc.nvFBCToGLSetUp(user_data->session, &(NVFBC_TOGL_SETUP_PARAMS) {
        .dwVersion = NVFBC_TOGL_SETUP_PARAMS_VER,
        .eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA
    });
    if (status) {
        blog(LOG_ERROR, "Failed to setup NvFBC ToGL capture: %d", status);
        return;
    }

    // get vulkan method pointer
    VkResult (*vkGetMemoryFdKHR)(VkDevice, const VkMemoryGetFdInfoKHR*, int*) = (void*) vkGetInstanceProcAddr(gstate.instance, "vkGetMemoryFdKHR");

    // hook textures
    int fd;
    int glstatus;
    for (int i = 0; i < 2; i++) {
        // grab vulkan memory fd
        vkGetMemoryFdKHR(gstate.device, &(VkMemoryGetFdInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .pNext = NULL,
            .memory = gstate.memory[i],
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
        }, &fd);

        // bind gl texture to vulkan memory
        glCreateMemoryObjectsEXT(1, &user_data->memory_objects[i]);
        if ((glstatus = glGetError())) {
            blog(LOG_ERROR, "Failed to create memory object: %d", glstatus);
            return;
        }
        glMemoryObjectParameterivEXT(user_data->memory_objects[i], GL_DEDICATED_MEMORY_OBJECT_EXT, &(GLint) { GL_TRUE });
        glImportMemoryFdEXT(user_data->memory_objects[i], gstate.size[i], GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        if ((glstatus = glGetError())) {
            blog(LOG_ERROR, "Failed to import memory fd: %d", glstatus);
            return;
        }
        glTextureStorageMem2DEXT(params->textures[i], 1, GL_RGBA8, params->frame_width, params->frame_height, user_data->memory_objects[i], 0);
        if ((glstatus = glGetError())) {
            blog(LOG_ERROR, "Failed to create texture storage: %d", glstatus);
            return;
        }
    }

    // release context
    status = fbc.nvFBCReleaseContext(user_data->session, &(NVFBC_RELEASE_CONTEXT_PARAMS) { .dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to release NvFBC context: %d", status);
        return;
    }

}

/**
 * Capture frame
 *
 * \author
 *   PancakeTAS
 *
 * \param params
 *   Capture parameters
 */
void capture_frame(capture_params* params) {
    nvfbc_user* user_data = (nvfbc_user*) params->user_data;

    // bind context
    NVFBCSTATUS status = fbc.nvFBCBindContext(user_data->session, &(NVFBC_BIND_CONTEXT_PARAMS) { .dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to bind NvFBC context: %d", status);
        return;
    }

    // capture frame
    NVFBC_TOGL_GRAB_FRAME_PARAMS grab_params = {
        .dwVersion = NVFBC_TOGL_GRAB_FRAME_PARAMS_VER,
        .dwFlags = NVFBC_TOGL_GRAB_FLAGS_NOWAIT
    };
    status = fbc.nvFBCToGLGrabFrame(user_data->session, &grab_params);
    if (status) {
        blog(LOG_ERROR, "Failed to grab NvFBC frame: %d", status);
        return;
    }

    // release context
    status = fbc.nvFBCReleaseContext(user_data->session, &(NVFBC_RELEASE_CONTEXT_PARAMS) { .dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to release NvFBC context: %d", status);
        return;
    }

    // switch textures
    params->current_texture = grab_params.dwTextureIndex;
}

/**
 * Stop capture
 *
 * \author
 *   PancakeTAS
 *
 * \param params
 *  Capture parameters
 */
void stop_capture(capture_params* params) {
    blog(LOG_INFO, "Stopping capture");
    nvfbc_user* user_data = (nvfbc_user*) params->user_data;

    // bind context
    NVFBCSTATUS status = fbc.nvFBCBindContext(user_data->session, &(NVFBC_BIND_CONTEXT_PARAMS) { .dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to bind NvFBC context: %d", status);
        return;
    }

    // destroy NvFBC capture session
    status = fbc.nvFBCDestroyCaptureSession(user_data->session, &(NVFBC_DESTROY_CAPTURE_SESSION_PARAMS) { .dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to destroy NvFBC capture session: %d", status);
        return;
    }

    // destroy NvFBC session
    status = fbc.nvFBCDestroyHandle(user_data->session, &(NVFBC_DESTROY_HANDLE_PARAMS) { .dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER });
    if (status) {
        blog(LOG_ERROR, "Failed to destroy NvFBC session: %d", status);
        return;
    }

    // free memory objects
    for (int i = 0; i < 2; i++) {
        glDeleteMemoryObjectsEXT(1, &user_data->memory_objects[i]);

    }

    // free user data
    free(user_data);
}

/**
 * Module load function
 *
 * \author
 *   PancakeTAS
 *
 * \return
 *   True if the module loaded successfully, false otherwise
 */
bool obs_module_load() {
    register_fbc_source(start_capture, capture_frame, stop_capture);

    // create NvFBC instance
    NVFBCSTATUS status = NvFBCCreateInstance(&fbc);
    if (status) {
        blog(LOG_ERROR, "Failed to create NvFBC instance: %d", status);
        return false;
    }

    // load function pointers
    glCreateMemoryObjectsEXT = (void*) eglGetProcAddress("glCreateMemoryObjectsEXT");
    glMemoryObjectParameterivEXT = (void*) eglGetProcAddress("glMemoryObjectParameterivEXT");
    glImportMemoryFdEXT = (void*) eglGetProcAddress("glImportMemoryFdEXT");
    glTextureStorageMem2DEXT = (void*) eglGetProcAddress("glTextureStorageMem2DEXT");
    glDeleteMemoryObjectsEXT = (void*) eglGetProcAddress("glDeleteMemoryObjectsEXT");

    return true;
}
