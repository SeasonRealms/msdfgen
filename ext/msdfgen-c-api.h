#pragma once

#include <stdint.h>

#ifndef MSDFGEN_EXT_PUBLIC
#define MSDFGEN_EXT_PUBLIC // for DLL import/export
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SMG_ABI_VERSION 1

enum smg_status {
    SMG_STATUS_SUCCESS = 0,
    SMG_STATUS_INVALID_ARGUMENT = 1,
    SMG_STATUS_FONT_ERROR = 2,
    SMG_STATUS_GLYPH_NOT_FOUND = 3,
    SMG_STATUS_SHAPE_ERROR = 4,
    SMG_STATUS_ALLOCATION_FAILED = 5,
    SMG_STATUS_INTERNAL_ERROR = 6
};

enum smg_coloring_strategy {
    SMG_COLORING_SIMPLE = 0,
    SMG_COLORING_INK_TRAP = 1,
    SMG_COLORING_BY_DISTANCE = 2
};

typedef struct smg_glyph_request {
    uint32_t struct_size;

    const uint8_t *font_data;
    int32_t font_data_size;
    const char *font_path;

    uint32_t codepoint;
    int32_t coordinate_scaling;

    double glyph_scale;
    double pixel_range;
    double angle_threshold;
    uint64_t coloring_seed;
    int32_t coloring_strategy;
    int32_t overlap_support;

    int32_t error_correction_mode;
    int32_t error_correction_distance_check_mode;
    double min_deviation_ratio;
    double min_improve_ratio;
} smg_glyph_request;

typedef struct smg_glyph_result {
    uint32_t struct_size;

    float *pixels;
    int32_t width;
    int32_t height;
    int32_t channels;
    int32_t y_axis_orientation;

    uint32_t glyph_index;
    double advance;
    double bounds_l;
    double bounds_b;
    double bounds_r;
    double bounds_t;
} smg_glyph_result;

MSDFGEN_EXT_PUBLIC int smg_get_abi_version(void);
MSDFGEN_EXT_PUBLIC const char *smg_get_last_error(void);
MSDFGEN_EXT_PUBLIC void smg_init_glyph_request(smg_glyph_request *request);
MSDFGEN_EXT_PUBLIC void smg_init_glyph_result(smg_glyph_result *result);
MSDFGEN_EXT_PUBLIC int smg_generate_glyph_mtsdf(const smg_glyph_request *request, smg_glyph_result *result);
MSDFGEN_EXT_PUBLIC void smg_free(void *ptr);

#ifdef __cplusplus
}
#endif
