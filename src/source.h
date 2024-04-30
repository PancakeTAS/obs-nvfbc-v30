#pragma once

#include <GL/gl.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int tracking_type; //!< Tracking type
    char display_name[256]; //!< Display name (for tracking type 1)
    bool has_capture_area; //!< Whether the source has a cropped capture area
    int capture_x, capture_y, capture_width, capture_height; //!< Capture area
    int frame_width, frame_height; //!< Frame size
    bool with_cursor; //!< Whether to capture the cursor
    bool push_model; //!< Whether to use the push model
    int sampling_rate; //!< Sampling rate in ms (only for tracking type 1)
    bool direct_mode; //!< Whether to allow direct mode

    GLuint textures[2]; //!< GL textures to render to
    int current_texture; //!< Pointer to the index of the texture to render

    void* user_data; //!< User data
} capture_params; //!< Capture parameters

/**
 * Register the source
 *
 * \author
 *   PancakeTAS
 *
 * \param start_callback
 *   Callback to start capturing
 * \param capture_callback
 *   Callback to capture a frame (shall not block)
 * \param stop_callback
 *   Callback to stop capturing
 */
void register_fbc_source(void(*start_callback)(capture_params*), void(*capture_callback)(capture_params*), void(*stop_callback)(capture_params*));
