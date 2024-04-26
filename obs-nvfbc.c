#include <NvFBC.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <threads.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <obs/obs-module.h>

#define __USE_GNU
#include <link.h>

OBS_DECLARE_MODULE()

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

    mtx_lock(&source_data->lock);
    gs_texture_t* texture = gs_texture_create(source_data->width, source_data->height, GS_BGRA, 1, (const uint8_t **) &source_data->frame_ptr, 0);
    mtx_unlock(&source_data->lock);

    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, texture);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, source_data->width, source_data->height);

    gs_texture_destroy(texture);
}

static int capture_thread(void* data) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    nvfbc_capture_t* capture_data = &source_data->capture_data;

    // create nvfbc
    NVFBC_API_FUNCTION_LIST fbc = { .dwVersion = NVFBC_VERSION };
    NVFBC_SESSION_HANDLE handle;
    NVFBC_GET_STATUS_PARAMS status = { .dwVersion = NVFBC_GET_STATUS_PARAMS_VER };
    NvFBCCreateInstance(&fbc);
    NvFBCCreateHandle(&handle, &(NVFBC_CREATE_HANDLE_PARAMS) { .dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER });
    NvFBCGetStatus(handle, &status);

    while (true) {
        if (!source_data->should_run) {
            thrd_sleep(&(struct timespec) { .tv_sec = 0, .tv_nsec = 10000000 }, NULL);
            continue;
        }

        // find display if using tracking type 1
        int dwOutputId = 0;
        if (capture_data->tracking_type == 1) {
            for (uint32_t i = 0; i < status.dwOutputNum; i++) {
                if (strncmp(capture_data->display_name, status.outputs[i].name, 127) == 0) {
                    dwOutputId = status.outputs[i].dwId;
                    break;
                }
            }
        }

        // create capture session
        NVFBC_CREATE_CAPTURE_SESSION_PARAMS session_params = {
            .dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
            .eCaptureType = NVFBC_CAPTURE_TO_SYS,
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
        fbc.nvFBCCreateCaptureSession(handle, &session_params);

        // setup capture session
        void* buffer;
        NVFBC_TOSYS_SETUP_PARAMS tosys_params = {
            .dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER,
            .eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA,
            .ppBuffer = &buffer
        };
        fbc.nvFBCToSysSetUp(handle, &tosys_params);

        // capture loop
        NVFBC_TOSYS_GRAB_FRAME_PARAMS tosys_grab_params = {
            .dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER,
            .dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY
        };
        while (source_data->should_run) {
            fbc.nvFBCToSysGrabFrame(handle, &tosys_grab_params);
            memcpy(source_data->frame_ptr, buffer, source_data->width * source_data->height * 4);
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
