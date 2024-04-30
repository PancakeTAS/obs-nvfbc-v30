
#include "source.h"

#include <obs/obs-module.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

typedef struct {
    obs_source_t* source; //!< OBS source
    gs_texture_t* textures[2]; //!< Texture to render to

    bool is_capturing; //!< Whether the source is capturing
    capture_params params; //!< Capture parameters
} fbc_source; //!< NvFBC source data

static void (*start_callback)(capture_params*); //!< Callback to start capturing
static void (*capture_callback)(capture_params*); //!< Callback to capture a frame
static void (*stop_callback)(capture_params*); //!< Callback to stop capturing

/**
 * Return name of the source
 *
 * \author
 *   PancakeTAS
 *
 * \return
 *   Name of the source
 */
static const char* get_name(void* unused) {
    return "NvFBC Source";
}

/**
 * Return width of the source
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 */
static uint32_t get_width(void* data) {
    return ((fbc_source*) data)->params.frame_width;
}

/**
 * Return height of the source
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 */
static uint32_t get_height(void* data) {
    return ((fbc_source*) data)->params.frame_height;
}

/**
 * Reload source on reload click
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 *
 * \return
 *  True if the source was reloaded successfully, false otherwise
 */
static bool on_reload(obs_properties_t*, obs_property_t *, void *data) {
    fbc_source* source_data = (fbc_source*) data;

    // stop the source
    obs_enter_graphics();
    if (source_data->is_capturing) {
        source_data->is_capturing = false;

        // close the textures
        gs_texture_destroy(source_data->textures[0]);
        gs_texture_destroy(source_data->textures[1]);

        stop_callback(&source_data->params);
    }
    obs_leave_graphics();

    // recreate capture params
    obs_data_t* settings = obs_source_get_settings(source_data->source);
    capture_params* params = &source_data->params;
    params->frame_width = obs_data_get_int(settings, "width");
    params->frame_height = obs_data_get_int(settings, "height");
    params->with_cursor = obs_data_get_bool(settings, "with_cursor");
    params->sampling_rate = obs_data_get_int(settings, "sampling_rate");
    params->push_model = obs_data_get_int(settings, "sampling_rate") == 0;

    const char* tracking_type = obs_data_get_string(settings, "tracking_type");
    if (tracking_type[0] != '0' && tracking_type[0] != '2') {
        // (output is specified)
        params->tracking_type = 1;
        strncpy(params->display_name, tracking_type, 127);
        strchr(params->display_name, ':')[0] = '\0';
    } else {
        params->tracking_type = tracking_type[0] - '0';
    }

    params->direct_mode = obs_data_get_bool(settings, "direct_capture");
    if (params->direct_mode) {
        params->with_cursor = false;
        params->push_model = true;
    }

    params->has_capture_area = obs_data_get_bool(settings, "crop_area");
    if (params->has_capture_area) {
        params->capture_x = obs_data_get_int(settings, "capture_x");
        params->capture_y = obs_data_get_int(settings, "capture_y");
        params->capture_width = obs_data_get_int(settings, "capture_width");
        params->capture_height = obs_data_get_int(settings, "capture_height");
    }

    // create textures
    obs_enter_graphics();
    for (int i = 0; i < 2; i++) {
        gs_texture_t* texture = gs_texture_create(params->frame_width, params->frame_height, GS_BGRA, 1, NULL, GS_DYNAMIC); // FIXME (potential bug): check if "NULL" works
        if (!texture) {
            blog(LOG_ERROR, "Failed to create texture for nvfbc obs source");
            return false;
        }

        GLuint gl_texture = *(GLuint*) gs_texture_get_obj(texture);
        glBindTexture(GL_TEXTURE_2D, gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glBindTexture(GL_TEXTURE_2D, 0);

        source_data->textures[i] = texture;
        source_data->params.textures[i] = gl_texture;
    }

    // start the source
    start_callback(&source_data->params);
    source_data->is_capturing = true;

    obs_leave_graphics();

    // cleanup
    obs_data_release(settings);

    return true;
}

/**
 * Update source data
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 * \param settings
 *   Settings of the source
 */
static void update(void* data, obs_data_t* settings) {
    fbc_source* source_data = (fbc_source*) data;

    // stop the source
    obs_enter_graphics();
    if (source_data->is_capturing) {
        source_data->is_capturing = false;

        // close the textures
        gs_texture_destroy(source_data->textures[0]);
        gs_texture_destroy(source_data->textures[1]);

        stop_callback(&source_data->params);
    }
    obs_leave_graphics();

    // then update the source data
    source_data->params.frame_width = obs_data_get_int(settings, "width");
    source_data->params.frame_height = obs_data_get_int(settings, "height");
}

/**
 * Create and update new source
 *
 * \author
 *   PancakeTAS
 *
 * \param settings
 *   Settings of the source
 * \param source
 *   OBS source
 */
static void* create(obs_data_t* settings, obs_source_t* source) {
    fbc_source* source_data = bzalloc(sizeof(fbc_source));
    source_data->source = source;

    update(source_data, settings);
    on_reload(NULL, NULL, source_data);
    return source_data;
}

/**
 * Render the source
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 * \param effect
 *   Current effect
 */
static void render(void* data, gs_effect_t* effect) {
    fbc_source* source_data = (fbc_source*) data;
    if (!source_data->is_capturing)
        return;

    // capture a frame
    capture_callback(&source_data->params);

    // TODO: check if this is how it's supposed to be done

    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, source_data->textures[source_data->params.current_texture]);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(source_data->textures[source_data->params.current_texture], 0, source_data->params.frame_width, source_data->params.frame_height);

}

/**
 * Update properties window on direct_update click
 *
 * \author
 *   PancakeTAS
 *
 * \param props
 *   Properties of the source
 * \param settings
 *   Settings of the source
 */
static bool on_direct_update(obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    obs_property_set_visible(obs_properties_get(props, "with_cursor"), !obs_data_get_bool(settings, "direct_capture"));
    obs_property_set_visible(obs_properties_get(props, "sampling_rate"), !obs_data_get_bool(settings, "direct_capture"));
    return true;
}

/**
 * Update properties window on crop click
 *
 * \author
 *   PancakeTAS
 *
 * \param props
 *   Properties of the source
 * \param settings
 *   Settings of the source
 */
static bool on_crop_update(obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    obs_property_set_visible(obs_properties_get(props, "capture_area"), obs_data_get_bool(settings, "crop_area"));
    return true;
}

/**
 * Return properties of the source
 *
 * \author
 *   PancakeTAS
 *
 * \return
 *   Properties of the source
 */
static obs_properties_t* get_properties(void* unused) {
    obs_properties_t* props = obs_properties_create();

    // tracking type
    obs_property_t* prop = obs_properties_add_list(props, "tracking_type", "Tracking Type", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(prop, "Primary Screen", "0");
    obs_property_list_add_string(prop, "Entire X Screen", "2");

    // TODO: replace with NvFBC
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

/**
 * Set default values for the source
 *
 * \author
 *   PancakeTAS
 *
 * \param settings
 *   Settings of the source
 */
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

/**
 * Destroy the source
 *
 * \author
 *   PancakeTAS
 *
 * \param data
 *   Source data
 */
static void destroy(void* data) {
    fbc_source* source_data = (fbc_source*) data;

    // stop the source
    obs_enter_graphics();
    if (source_data->is_capturing) {
        source_data->is_capturing = false;

        // close the textures
        gs_texture_destroy(source_data->textures[0]);
        gs_texture_destroy(source_data->textures[1]);

        stop_callback(&source_data->params);
    }
    obs_leave_graphics();

    bfree(data);
}

/// Struct describing the NvFBC source
static struct obs_source_info nvfbc_source = {
    .id = "nvfbc-source",
    .version = 1,
    .get_name = get_name,

    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,

    .create = create,
    .update = update,
    .destroy = destroy,
    .video_render = render,

    .get_properties = get_properties,
    .get_defaults = get_defaults,

    .get_width = get_width,
    .get_height = get_height,
};

void register_fbc_source(void(*new_start_callback)(capture_params*), void(*new_capture_callback)(capture_params*), void(*new_stop_callback)(capture_params*)) {
    start_callback = new_start_callback;
    capture_callback = new_capture_callback;
    stop_callback = new_stop_callback;
    obs_register_source(&nvfbc_source);
}
