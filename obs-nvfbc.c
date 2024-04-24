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
    int width, height; //!< Resolution of the OBS source

    bool is_running; //!< Is the NvFBC subprocess running
    pid_t subprocess_pid; //!< NvFBC subprocess PID
    char shm_name[32]; //!< Shared memory name
    int shm_fd; //!< Shared memory file descriptor
    void* frame_ptr; //!< Frame buffer pointer

    obs_source_t* source; //!< OBS source
    mtx_t lock; //!< Lock to prevent rendering while updating the frame buffer
} nvfbc_source_t; //!< Source data for the NvFBC source

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

static volatile int dl_it = 0; //!< Hack to get the path to the shared object

static void start_source(void* data, obs_data_t* settings) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (source_data->is_running) return;

    mtx_lock(&source_data->lock);

    // create shared memory
    snprintf(source_data->shm_name, 32, "obs-nvfbc-%d", rand() % 256);
    source_data->shm_fd = shm_open(source_data->shm_name, O_RDWR | O_CREAT, 0666);
    if (source_data->shm_fd == -1) {
        blog(LOG_ERROR, "Failed to open shared memory: %s", strerror(errno));

        mtx_unlock(&source_data->lock);
        return;
    }

    // truncate shared memory
    if (ftruncate(source_data->shm_fd, source_data->width * source_data->height * 4) == -1) {
        blog(LOG_ERROR, "Failed to truncate shared memory: %s", strerror(errno));

        close(source_data->shm_fd);
        shm_unlink(source_data->shm_name);
        mtx_unlock(&source_data->lock);
        return;
    }

    // map shared memory
    source_data->frame_ptr = mmap(NULL, source_data->width * source_data->height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, source_data->shm_fd, 0);
    if (source_data->frame_ptr == MAP_FAILED) {
        blog(LOG_ERROR, "Failed to map shared memory: %s", strerror(errno));

        close(source_data->shm_fd);
        shm_unlink(source_data->shm_name);
        mtx_unlock(&source_data->lock);
        return;
    }

    // prepare shared memory
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

    memcpy(source_data->frame_ptr, &capture_data, sizeof(nvfbc_capture_t));
    blog(LOG_INFO, "Launching subprocess with: { tracking_type: %d, display_name: %s, capture_x: %d, capture_y: %d, capture_width: %d, capture_height: %d, frame_width: %d, frame_height: %d, with_cursor: %d, push_model: %d, sampling_rate: %d }",
        capture_data.tracking_type, capture_data.display_name, capture_data.capture_x, capture_data.capture_y, capture_data.capture_width, capture_data.capture_height, capture_data.frame_width, capture_data.frame_height, capture_data.with_cursor, capture_data.push_model, capture_data.sampling_rate);

    // launch subprocess
    pid_t pid = fork();
    if (pid == 0) {
        // get path to current shared object
        Dl_info dlinfo;
        dladdr((const void*) &dl_it, &dlinfo);

        // launch self as subprocess
        char* path = strdup(dlinfo.dli_fname);
        setenv("SHM_NAME", source_data->shm_name, 1);
        extern char** environ;
        execve(path, (char* const[]) { path, NULL }, environ);
        return;
    }

    source_data->subprocess_pid = pid;
    source_data->is_running = true;

    mtx_unlock(&source_data->lock);
}

static void stop_source(void* data) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (!source_data->is_running) return;
    mtx_lock(&source_data->lock);
    source_data->is_running = false;

    // close shared memory
    munmap(source_data->frame_ptr, source_data->width * source_data->height * 4);
    close(source_data->shm_fd);
    shm_unlink(source_data->shm_name);

    // kill subprocess
    kill(source_data->subprocess_pid, SIGINT);
    waitpid(source_data->subprocess_pid, NULL, 0);
    mtx_unlock(&source_data->lock);
}

// rendering stuff

static void video_render(void* data, gs_effect_t* effect) {
    nvfbc_source_t* source_data = (nvfbc_source_t*) data;
    if (!source_data->is_running) return;

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

void child_died(int sig) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

bool obs_module_load(void) {
    signal(SIGCHLD, child_died);
    obs_register_source(&nvfbc_source);
    return true;
}

// executable stuff

void nvfbc_main() {
    printf("Shared memory name: %s\n", getenv("SHM_NAME"));

    // open shared memory
    int shm_fd = shm_open(getenv("SHM_NAME"), O_RDWR, 0666);
    if (shm_fd == -1) {
        printf("Failed to open shared memory: %s\n", strerror(errno));
        exit(1);
    }

    // copy struct
    nvfbc_capture_t nvfbc;
    void* pointer = mmap(NULL, sizeof(nvfbc), PROT_READ, MAP_SHARED, shm_fd, 0);
    if (pointer == MAP_FAILED) {
        printf("Failed to map shared memory: %s\n", strerror(errno));
        exit(1);
    }
    memcpy(&nvfbc, pointer, sizeof(nvfbc));

    // reopen shared memory with full size
    munmap(pointer, sizeof(nvfbc));
    pointer = mmap(NULL, nvfbc.frame_width * nvfbc.frame_height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pointer == MAP_FAILED) {
        printf("Failed to map shared memory: %s\n", strerror(errno));
        exit(1);
    }
    printf("Mapped shared memory\n");

    // create nvfbc instance
    NVFBC_API_FUNCTION_LIST fbc = { .dwVersion = NVFBC_VERSION };

    NVFBCSTATUS status = NvFBCCreateInstance(&fbc);
    if (status) {
        printf("NvFBCCreateInstance() failed: %d\n", status);
        exit(1);
    }

    NVFBC_SESSION_HANDLE handle;
    status = fbc.nvFBCCreateHandle(&handle, &(NVFBC_CREATE_HANDLE_PARAMS) { .dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER });
    if (status) {
        printf("nvFBCCreateHandle() failed: %d\n", status);
        exit(1);
    }

    printf("Created NvFBC instance\n");

    // get status params if needed
    int dwOutputId = 0;
    if (nvfbc.tracking_type == 1) {
        NVFBC_GET_STATUS_PARAMS status_params = { .dwVersion = NVFBC_GET_STATUS_PARAMS_VER };
        status = fbc.nvFBCGetStatus(handle, &status_params);
        if (status) {
            printf("nvFBCGetStatus() failed: %s (%d)\n", fbc.nvFBCGetLastErrorStr(handle), status);
            exit(1);
        }

        // find output id
        for (uint32_t i = 0; i < status_params.dwOutputNum; i++) {
            if (strcmp(nvfbc.display_name, status_params.outputs[i].name) == 0) {
                dwOutputId = status_params.outputs[i].dwId;
                break;
            }
        }
        printf("Found output id: %d\n", dwOutputId);
    }

    // create capture session
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS session_params = {
        .dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
        .eCaptureType = NVFBC_CAPTURE_TO_SYS,
        .bWithCursor = nvfbc.with_cursor,
        .eTrackingType = nvfbc.tracking_type,
        .frameSize = { .w = nvfbc.frame_width, .h = nvfbc.frame_height },
        .dwOutputId = dwOutputId,
        .dwSamplingRateMs = nvfbc.sampling_rate,
        .bPushModel = nvfbc.push_model,
        .bAllowDirectCapture = nvfbc.direct_capture
    };
    if (nvfbc.has_capture_area) {
        session_params.captureBox.x = nvfbc.capture_x;
        session_params.captureBox.y = nvfbc.capture_y;
        session_params.captureBox.w = nvfbc.capture_width;
        session_params.captureBox.h = nvfbc.capture_height;
    }
    status = fbc.nvFBCCreateCaptureSession(handle, &session_params);
    if (status) {
        printf("nvFBCCreateCaptureSession() failed: %s (%d)\n", fbc.nvFBCGetLastErrorStr(handle), status);
        exit(1);
    }

    void* buffer;
    NVFBC_TOSYS_SETUP_PARAMS tosys_params = {
        .dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER,
        .eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA,
        .ppBuffer = &buffer
    };
    status = fbc.nvFBCToSysSetUp(handle, &tosys_params);
    if (status) {
        printf("nvFBCToSysSetUp() failed: %s (%d)\n", fbc.nvFBCGetLastErrorStr(handle), status);
        exit(1);
    }

    printf("Created capture session\n");

    // capture loop
    NVFBC_TOSYS_GRAB_FRAME_PARAMS tosys_grab_params = {
        .dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER,
        .dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY
    };
    while (1) {

        // grab frame
        status = fbc.nvFBCToSysGrabFrame(handle, &tosys_grab_params);
        if (status) {
            printf("nvFBCToSysGrabFrame() failed: %s (%d)\n", fbc.nvFBCGetLastErrorStr(handle), status);
            exit(1);
        }

        // copy frame
        memcpy(pointer, buffer, nvfbc.frame_width * nvfbc.frame_height * 4);

    }

    exit(0);
}

// (warning very hacky stuff ahead)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

const char my_interp[] __attribute__((section(".interp"))) = "/lib64/ld-linux-x86-64.so.2";

void* stack_ptr;
ucontext_t ctx, old_ctx;

int lib_main(int argc, char** argv) {
    // replace stack
    stack_ptr = malloc(1024 * 1024);
    getcontext(&ctx);
    ctx.uc_stack.ss_sp = stack_ptr;
    ctx.uc_stack.ss_size = 1024 * 1024;
    ctx.uc_link = &old_ctx;
    makecontext(&ctx, nvfbc_main, 0);
    swapcontext(&old_ctx, &ctx);
    return 0;
}

#pragma GCC diagnostic pop
