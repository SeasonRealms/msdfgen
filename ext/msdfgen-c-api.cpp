#include "msdfgen-c-api.h"

#include "../core/BitmapRef.hpp"
#include "../core/Projection.h"
#include "../core/Range.hpp"
#include "../core/Shape.h"
#include "../core/edge-coloring.h"
#include "../core/generator-config.h"
#include "../msdfgen.h"
#include "import-font.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>

using namespace msdfgen;

namespace {

thread_local std::string g_lastError;

void smgSetLastError(const char *message) {
    g_lastError = message ? message : "Unknown error.";
}

void smgSetLastError(const std::string &message) {
    g_lastError = message;
}

int smgFail(int status, const char *message) {
    smgSetLastError(message);
    return status;
}

void smgResetResult(smg_glyph_result *result) {
    if (!result)
        return;
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->channels = 4;
    result->y_axis_orientation = Y_UPWARD;
}

void smgApplyColoring(Shape &shape, const smg_glyph_request &request) {
    switch (request.coloring_strategy) {
        case SMG_COLORING_SIMPLE:
            edgeColoringSimple(shape, request.angle_threshold, request.coloring_seed);
            break;
        case SMG_COLORING_INK_TRAP:
            edgeColoringInkTrap(shape, request.angle_threshold, request.coloring_seed);
            break;
        case SMG_COLORING_BY_DISTANCE:
            edgeColoringByDistance(shape, request.angle_threshold, request.coloring_seed);
            break;
        default:
            edgeColoringInkTrap(shape, request.angle_threshold, request.coloring_seed);
            break;
    }
}

ErrorCorrectionConfig smgMakeErrorCorrectionConfig(const smg_glyph_request &request) {
    const double minDeviationRatio = request.min_deviation_ratio > 0 ? request.min_deviation_ratio : ErrorCorrectionConfig::defaultMinDeviationRatio;
    const double minImproveRatio = request.min_improve_ratio > 0 ? request.min_improve_ratio : ErrorCorrectionConfig::defaultMinImproveRatio;
    return ErrorCorrectionConfig(
        static_cast<ErrorCorrectionConfig::Mode>(request.error_correction_mode),
        static_cast<ErrorCorrectionConfig::DistanceCheckMode>(request.error_correction_distance_check_mode),
        minDeviationRatio,
        minImproveRatio
    );
}

} // namespace

extern "C" {

int smg_get_abi_version(void) {
    return SMG_ABI_VERSION;
}

uint32_t smg_sizeof_glyph_request(void) {
    return (uint32_t) sizeof(smg_glyph_request);
}

uint32_t smg_sizeof_glyph_result(void) {
    return (uint32_t) sizeof(smg_glyph_result);
}

const char *smg_get_last_error(void) {
    return g_lastError.empty() ? "" : g_lastError.c_str();
}

void smg_init_glyph_request(smg_glyph_request *request) {
    if (!request || (request->struct_size != 0 && request->struct_size < sizeof(*request)))
        return;
    request->struct_size = sizeof(*request);
}

void smg_init_glyph_result(smg_glyph_result *result) {
    if (!result || (result->struct_size != 0 && result->struct_size < sizeof(*result)))
        return;
    result->struct_size = sizeof(*result);
}

int smg_generate_glyph_mtsdf(const smg_glyph_request *request, smg_glyph_result *result) {
    smgSetLastError("");

    if (!request || !result)
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "Request/result pointer is null.");
    if (request->struct_size != 0 && request->struct_size < sizeof(*request))
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "Glyph request struct_size is smaller than expected.");
    if (result->struct_size != 0 && result->struct_size < sizeof(*result))
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "Glyph result struct_size is smaller than expected.");

    smgResetResult(result);

    if (request->glyph_scale <= 0)
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "glyph_scale must be greater than 0.");
    if (request->pixel_range < 0)
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "pixel_range must be non-negative.");
    if ((!request->font_data || request->font_data_size <= 0) && !request->font_path)
        return smgFail(SMG_STATUS_INVALID_ARGUMENT, "Either font_data or font_path must be provided.");

    FreetypeHandle *library = NULL;
    FontHandle *font = NULL;
    float *pixels = NULL;
    int status = SMG_STATUS_SUCCESS;

    try {
        do {
            library = initializeFreetype();
            if (!library) {
                status = smgFail(SMG_STATUS_FONT_ERROR, "initializeFreetype failed.");
                break;
            }

            if (request->font_data && request->font_data_size > 0)
                font = loadFontData(library, reinterpret_cast<const byte *>(request->font_data), request->font_data_size);
            else
                font = loadFont(library, request->font_path);

            if (!font) {
                status = smgFail(SMG_STATUS_FONT_ERROR, "Failed to load font.");
                break;
            }

            GlyphIndex glyphIndex;
            if (!getGlyphIndex(glyphIndex, font, static_cast<unicode_t>(request->codepoint))) {
                status = smgFail(SMG_STATUS_GLYPH_NOT_FOUND, "Glyph not found for requested codepoint.");
                break;
            }

            Shape shape;
            double advance = 0;
            if (!loadGlyph(shape, font, glyphIndex, static_cast<FontCoordinateScaling>(request->coordinate_scaling), &advance)) {
                status = smgFail(SMG_STATUS_FONT_ERROR, "loadGlyph failed.");
                break;
            }

            result->glyph_index = glyphIndex.getIndex();
            result->advance = advance;

            if (shape.contours.empty())
                break;

            if (!shape.validate()) {
                status = smgFail(SMG_STATUS_SHAPE_ERROR, "Loaded glyph shape is invalid.");
                break;
            }

            shape.orientContours();
            shape.normalize();
            smgApplyColoring(shape, *request);

            const Shape::Bounds bounds = shape.getBounds();
            const double border = request->pixel_range / request->glyph_scale;
            const double scale = request->glyph_scale;
            const int width = static_cast<int>(std::ceil((bounds.r-bounds.l+2.0*border)*scale));
            const int height = static_cast<int>(std::ceil((bounds.t-bounds.b+2.0*border)*scale));

            result->bounds_l = bounds.l;
            result->bounds_b = bounds.b;
            result->bounds_r = bounds.r;
            result->bounds_t = bounds.t;
            result->width = width;
            result->height = height;

            if (width <= 0 || height <= 0)
                break;

            Bitmap<float, 4> generated(width, height, Y_UPWARD);
            const Projection projection(
                Vector2(scale, scale),
                Vector2(border-bounds.l, border-bounds.b)
            );
            const Range range(request->pixel_range/request->glyph_scale);
            const MSDFGeneratorConfig config(request->overlap_support != 0, smgMakeErrorCorrectionConfig(*request));
            generateMTSDF(generated, shape, projection, range, config);

            const size_t pixelCount = static_cast<size_t>(width)*static_cast<size_t>(height)*4u;
            pixels = static_cast<float *>(std::malloc(pixelCount*sizeof(float)));
            if (!pixels) {
                status = smgFail(SMG_STATUS_ALLOCATION_FAILED, "Failed to allocate output pixel buffer.");
                break;
            }
            std::memcpy(pixels, static_cast<const float *>(generated), pixelCount*sizeof(float));

            result->pixels = pixels;
            result->channels = 4;
            result->y_axis_orientation = Y_UPWARD;
            pixels = NULL;
            status = SMG_STATUS_SUCCESS;
        } while (false);
    } catch (const std::bad_alloc &) {
        status = smgFail(SMG_STATUS_ALLOCATION_FAILED, "Memory allocation failed inside msdfgen bridge.");
    } catch (const std::exception &ex) {
        status = smgFail(SMG_STATUS_INTERNAL_ERROR, ex.what());
    } catch (...) {
        status = smgFail(SMG_STATUS_INTERNAL_ERROR, "Unknown exception inside msdfgen bridge.");
    }
    if (pixels)
        std::free(pixels);
    if (font)
        destroyFont(font);
    if (library)
        deinitializeFreetype(library);
    return status;
}

void smg_free(void *ptr) {
    std::free(ptr);
}

} // extern "C"
