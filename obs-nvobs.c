#include <NvFBC.h>

#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

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
} nvobs_source_t; //!< Source data for the NvFBC source

typedef struct {
    int tracking_type; //!< Tracking type (default, output, screen)
    char display_name[128]; //!< Display name (only if tracking_type is output)
    int capture_x, capture_y, capture_width, capture_height; // < Capture area
    int frame_width, frame_height; //!< Frame size
    bool with_cursor; //!< Capture cursor
    bool push_model; //!< Push model
    int sampling_rate; //!< Sampling rate in ms (only if push_model is disabled)
} nvobs_capture_t; //!< Capture data sent to subprocess for the NvFBC source

static volatile int dl_it = 0; //!< Hack to get the path to the shared object

static void start_source(void* data, obs_data_t* settings) {
    nvobs_source_t* source_data = (nvobs_source_t*) data;
    if (source_data->is_running) return;

    // create shared memory
    snprintf(source_data->shm_name, 32, "obs-nvfbc-%d", rand() % 256);
    source_data->shm_fd = shm_open(source_data->shm_name, O_RDWR | O_CREAT, 0666);
    if (source_data->shm_fd == -1) {
        blog(LOG_ERROR, "Failed to open shared memory: %s", strerror(errno));
        return;
    }

    // truncate shared memory
    if (ftruncate(source_data->shm_fd, source_data->width * source_data->height * 4) == -1) {
        blog(LOG_ERROR, "Failed to truncate shared memory: %s", strerror(errno));

        close(source_data->shm_fd);
        shm_unlink(source_data->shm_name);
        return;
    }

    // map shared memory
    source_data->frame_ptr = mmap(NULL, source_data->width * source_data->height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, source_data->shm_fd, 0);
    if (source_data->frame_ptr == MAP_FAILED) {
        blog(LOG_ERROR, "Failed to map shared memory: %s", strerror(errno));

        close(source_data->shm_fd);
        shm_unlink(source_data->shm_name);
        return;
    }

    // prepare shared memory
    nvobs_capture_t capture_data = {
        .tracking_type = obs_data_get_int(settings, "tracking_type"),
        .capture_x = obs_data_get_int(settings, "capture_x"),
        .capture_y = obs_data_get_int(settings, "capture_y"),
        .capture_width = obs_data_get_int(settings, "capture_width"),
        .capture_height = obs_data_get_int(settings, "capture_height"),
        .frame_width = obs_data_get_int(settings, "width"),
        .frame_height = obs_data_get_int(settings, "height"),
        .with_cursor = obs_data_get_bool(settings, "with_cursor"),
        .push_model = obs_data_get_bool(settings, "push_model"),
        .sampling_rate = obs_data_get_int(settings, "sampling_rate")
    };
    strncpy(capture_data.display_name, obs_data_get_string(settings, "display_name"), 127);
    memcpy(source_data->frame_ptr, &capture_data, sizeof(nvobs_capture_t));
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
        path[strlen(path) - 2] = 'o';
        path[strlen(path) - 1] = '\0';
        execvp(path, (char* const[]) { path, source_data->shm_name, NULL });
        return;
    }

    source_data->subprocess_pid = pid;
    source_data->is_running = true;
}

static void stop_source(void* data) {
    nvobs_source_t* source_data = (nvobs_source_t*) data;
    if (!source_data->is_running) return;

    // close shared memory
    munmap(source_data->frame_ptr, source_data->width * source_data->height * 4);
    close(source_data->shm_fd);
    shm_unlink(source_data->shm_name);

    // kill subprocess
    kill(source_data->subprocess_pid, SIGINT);
    source_data->is_running = false;
}

// rendering stuff

static void video_render(void* data, gs_effect_t* effect) {
    nvobs_source_t* source_data = (nvobs_source_t*) data;
    if (!source_data->is_running) return;

    // TODO: fix all this

    gs_texture_t* texture = gs_texture_create(source_data->width, source_data->height, GS_BGRA, 1, (const uint8_t **) &source_data->frame_ptr, 0);
    effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, texture);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, source_data->width, source_data->height);

    gs_texture_destroy(texture);
}

// obs configuration stuff

static bool on_tracking_update(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    obs_property_set_enabled(obs_properties_get(props, "display_name"), obs_data_get_int(settings, "tracking_type") == 1);
    return true;
}

static bool on_push_model_update(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    obs_property_set_visible(obs_properties_get(props, "sampling_rate"), !obs_data_get_bool(settings, "push_model"));
    return true;
}

static bool on_reload(obs_properties_t* unused, obs_property_t *unused1, void *data) {
    stop_source(data);
    start_source(data, obs_source_get_settings(((nvobs_source_t*) data)->source));
    return true;
}

static obs_properties_t* get_properties(void* unused) {
    obs_properties_t* props = obs_properties_create();

    // tracking type
    obs_property_t* prop = obs_properties_add_list(props, "tracking_type", "Tracking Type", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_set_modified_callback(prop, on_tracking_update);
    obs_property_list_add_int(prop, "Default", 0);
    obs_property_list_add_int(prop, "Output", 1);
    obs_property_list_add_int(prop, "Screen", 2);
    obs_properties_add_text(props, "display_name", "Display Name", OBS_TEXT_DEFAULT);

    // capture area
    obs_properties_t* capture_props = obs_properties_create();
    obs_properties_add_int(capture_props, "capture_x", "Capture X", 0, 4096, 2);
    obs_properties_add_int(capture_props, "capture_y", "Capture Y", 0, 4096, 2);
    obs_properties_add_int(capture_props, "capture_width", "Capture Width", 0, 4096, 2);
    obs_properties_add_int(capture_props, "capture_height", "Capture Height", 0, 4096, 2);
    obs_properties_add_group(props, "capture_area", "Capture Area", OBS_GROUP_NORMAL, capture_props);

    // frame size
    obs_properties_add_int(props, "width", "Width", 0, 4096, 2);
    obs_properties_add_int(props, "height", "Height", 0, 4096, 2);

    // misc capture options
    obs_properties_add_bool(props, "with_cursor", "With Cursor");

    // push model
    prop = obs_properties_add_bool(props, "push_model", "Push Model");
    obs_property_set_modified_callback(prop, on_push_model_update);
    obs_properties_add_int(props, "sampling_rate", "Sampling Rate", 0, 1000, 1);

    obs_properties_add_button(props, "settings", "Update settings", on_reload);

    return props;
}

static void get_defaults(obs_data_t* settings) {
    // tracking type
    obs_data_set_default_int(settings, "tracking_type", 0);
    obs_data_set_default_string(settings, "display_name", "");

    // capture area
    obs_data_set_default_int(settings, "capture_x", 0);
    obs_data_set_default_int(settings, "capture_y", 0);
    obs_data_set_default_int(settings, "capture_width", 1920);
    obs_data_set_default_int(settings, "capture_height", 1080);

    // frame size
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 1080);

    // misc capture options
    obs_data_set_default_bool(settings, "with_cursor", true);

    // push model
    obs_data_set_default_bool(settings, "push_model", false);
    obs_data_set_default_int(settings, "sampling_rate", 16);
}

// obs module essentials

static void update(void* data, obs_data_t* settings) {
    nvobs_source_t* source_data = (nvobs_source_t*) data;
    source_data->width = obs_data_get_int(settings, "width");
    source_data->height = obs_data_get_int(settings, "height");

    stop_source(data);
}

const char* get_name(void* unused) { return "NvFBC Source"; }
static uint32_t get_width(void* data) { return ((nvobs_source_t*) data)->width; }
static uint32_t get_height(void* data) { return ((nvobs_source_t*) data)->height; }

static void* create(obs_data_t* settings, obs_source_t* source) {
    nvobs_source_t* source_data = (nvobs_source_t*) bzalloc(sizeof(nvobs_source_t));
    source_data->source = source;
    update(source_data, settings);
    start_source(source_data, settings);
    return source_data;
}

static void destroy(void* data) {
    stop_source(data);
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

// executable stuff

int main(int argc, char** argv) {
    // open shared memory
    int shm_fd = shm_open(argv[1], O_RDWR, 0666);
    if (shm_fd == -1) {
        printf("Failed to open shared memory: %s\n", strerror(errno));
        exit(1);
    }

    // copy struct
    nvobs_capture_t nvfbc;
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
        .frameSize = { .w = nvfbc.frame_width, .h = nvfbc.frame_height },
        .captureBox = { .x = nvfbc.capture_x, .y = nvfbc.capture_y, .w = nvfbc.capture_width, .h = nvfbc.capture_height },
        .eTrackingType = nvfbc.tracking_type,
        .dwOutputId = dwOutputId,
        .dwSamplingRateMs = nvfbc.sampling_rate,
        .bPushModel = nvfbc.push_model
    };
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
        .dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS
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
    return 0;

}
