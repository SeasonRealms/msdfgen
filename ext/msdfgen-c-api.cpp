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
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <string>

using namespace msdfgen;

namespace {

thread_local std::string g_lastError;
std::mutex g_logMutex;

const char *SMG_COMPARE_LOG_DIR_ENV = "SEASON_MSDF_COMPARE_LOG_DIR";
const unsigned SMG_TARGET_CODEPOINTS[] = {
    0x4E2D, // 中
    0x534E, // 华
    0x6574, // 整
    0x6700, // 最
    0x53D7, // 受
    0x675F, // 束
    0x8FC7, // 过
    0x004C, // L
    0x0031, // 1
    0x0032, // 2
    0x0030  // 0
};

std::string smgFormatDouble(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string smgFormatPoint(double x, double y) {
    return smgFormatDouble(x)+","+smgFormatDouble(y);
}

std::string smgFormatBounds(double l, double b, double r, double t) {
    return smgFormatDouble(l)+","+smgFormatDouble(b)+","+smgFormatDouble(r)+","+smgFormatDouble(t);
}

std::string smgFormatYAxis(YAxisOrientation orientation) {
    return orientation == Y_UPWARD ? "Y_UPWARD" : "Y_DOWNWARD";
}

bool smgShouldLogCodepoint(unsigned codepoint) {
    for (unsigned target : SMG_TARGET_CODEPOINTS) {
        if (target == codepoint)
            return true;
    }
    return false;
}

std::string smgGetNativeLogPath() {
    const char *logDir = std::getenv(SMG_COMPARE_LOG_DIR_ENV);
    if (!logDir || !*logDir)
        return std::string();

    std::string path = logDir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path.push_back('\\');
    path += "native-log.txt";
    return path;
}

void smgAppendCompareLine(const std::string &line) {
    const std::string path = smgGetNativeLogPath();
    if (path.empty())
        return;

    std::lock_guard<std::mutex> guard(g_logMutex);
    std::ofstream stream(path.c_str(), std::ios::out|std::ios::app|std::ios::binary);
    if (!stream)
        return;
    stream << line << "\n";
}

std::string smgComputeByteHash(const void *data, size_t size) {
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    unsigned long long hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string smgComputeFloatHash(const float *data, size_t count) {
    return smgComputeByteHash(data, count*sizeof(float));
}

std::string smgCreatePrefix(const char *kind, const char *stage, unsigned codepoint, int glyphIndex) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "kind=" << kind
           << "|stage=" << stage
           << "|backend=native"
           << "|codepoint=U+"
           << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << codepoint
           << std::dec
           << "|glyph_index=" << glyphIndex;
    return stream.str();
}

std::string smgFormatControlPoints(const EdgeSegment &segment) {
    const Point2 *points = segment.controlPoints();
    const int pointCount = segment.type()+1;
    std::string result;
    for (int i = 0; i < pointCount; ++i) {
        if (i)
            result.push_back(';');
        result += smgFormatPoint(points[i].x, points[i].y);
    }
    return result;
}

std::string smgComputeShapeHash(const Shape &shape) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "y_axis=" << smgFormatYAxis(shape.getYAxisOrientation());
    stream << ";contours=" << shape.contours.size();
    Shape::Bounds bounds = shape.getBounds();
    stream << ";bounds=" << smgFormatBounds(bounds.l, bounds.b, bounds.r, bounds.t);
    for (size_t contourIndex = 0; contourIndex < shape.contours.size(); ++contourIndex) {
        const Contour &contour = shape.contours[contourIndex];
        stream << ";c" << contourIndex
               << ":w=" << contour.winding()
               << ":e=" << contour.edges.size();
        for (size_t edgeIndex = 0; edgeIndex < contour.edges.size(); ++edgeIndex) {
            const EdgeSegment &segment = *contour.edges[edgeIndex];
            stream << ";e" << edgeIndex
                   << ":t=" << segment.type()
                   << ":c=" << int(segment.color)
                   << ":p=" << smgFormatControlPoints(segment);
        }
    }
    const std::string text = stream.str();
    return smgComputeByteHash(text.data(), text.size());
}

void smgLogRequest(const smg_glyph_request &request, int glyphIndex) {
    if (!smgShouldLogCodepoint(request.codepoint))
        return;

    std::string line = smgCreatePrefix("request", "S00.Request", request.codepoint, glyphIndex);
    line += "|font=memory";
    line += "|font_size=-1";
    line += "|units_per_em=-1";
    line += "|font_bytes="+std::to_string(request.font_data_size > 0 ? request.font_data_size : 0);
    line += "|font_hash=";
    line += request.font_data && request.font_data_size > 0
        ? smgComputeByteHash(request.font_data, size_t(request.font_data_size))
        : std::string("NA");
    line += "|glyph_scale="+smgFormatDouble(request.glyph_scale);
    line += "|pixel_range="+smgFormatDouble(request.pixel_range);
    line += "|angle_threshold="+smgFormatDouble(request.angle_threshold);
    line += "|coloring_seed="+std::to_string(request.coloring_seed);
    line += "|coloring_strategy="+std::to_string(request.coloring_strategy);
    line += "|overlap_support="+std::to_string(request.overlap_support != 0 ? 1 : 0);
    line += "|coord_scaling="+std::to_string(request.coordinate_scaling);
    smgAppendCompareLine(line);
}

void smgLogShapeStage(const char *stage, unsigned codepoint, int glyphIndex, const Shape &shape) {
    if (!smgShouldLogCodepoint(codepoint))
        return;

    Shape::Bounds bounds = shape.getBounds();
    std::string windingValues;
    for (size_t i = 0; i < shape.contours.size(); ++i) {
        if (i)
            windingValues.push_back(',');
        windingValues += std::to_string(shape.contours[i].winding());
    }

    std::string summary = smgCreatePrefix("shape", stage, codepoint, glyphIndex);
    summary += "|contours="+std::to_string(shape.contours.size());
    summary += "|edges="+std::to_string(shape.edgeCount());
    summary += "|bounds="+smgFormatBounds(bounds.l, bounds.b, bounds.r, bounds.t);
    summary += "|y_axis="+smgFormatYAxis(shape.getYAxisOrientation());
    summary += "|winding="+windingValues;
    summary += "|shape_hash="+smgComputeShapeHash(shape);
    smgAppendCompareLine(summary);

    for (size_t contourIndex = 0; contourIndex < shape.contours.size(); ++contourIndex) {
        const Contour &contour = shape.contours[contourIndex];
        std::string contourLine = smgCreatePrefix("contour", stage, codepoint, glyphIndex);
        contourLine += "|contour="+std::to_string(contourIndex);
        contourLine += "|winding="+std::to_string(contour.winding());
        contourLine += "|edges="+std::to_string(contour.edges.size());
        smgAppendCompareLine(contourLine);

        for (size_t edgeIndex = 0; edgeIndex < contour.edges.size(); ++edgeIndex) {
            const EdgeSegment &segment = *contour.edges[edgeIndex];
            std::string edgeLine = smgCreatePrefix("edge", stage, codepoint, glyphIndex);
            edgeLine += "|contour="+std::to_string(contourIndex);
            edgeLine += "|edge="+std::to_string(edgeIndex);
            edgeLine += "|type="+std::to_string(segment.type());
            edgeLine += "|color="+std::to_string(int(segment.color));
            edgeLine += "|points="+smgFormatControlPoints(segment);
            smgAppendCompareLine(edgeLine);
        }
    }
}

void smgLogProjectionStage(
    const char *stage,
    unsigned codepoint,
    int glyphIndex,
    const Shape::Bounds &bounds,
    double border,
    double scale,
    const Vector2 &translate,
    double range,
    int width,
    int height,
    double advance) {
    if (!smgShouldLogCodepoint(codepoint))
        return;

    std::string line = smgCreatePrefix("projection", stage, codepoint, glyphIndex);
    line += "|bounds="+smgFormatBounds(bounds.l, bounds.b, bounds.r, bounds.t);
    line += "|border="+smgFormatDouble(border);
    line += "|scale="+smgFormatDouble(scale);
    line += "|translate="+smgFormatPoint(translate.x, translate.y);
    line += "|range="+smgFormatDouble(range);
    line += "|width="+std::to_string(width);
    line += "|height="+std::to_string(height);
    line += "|advance="+smgFormatDouble(advance);
    smgAppendCompareLine(line);
}

void smgLogBitmapStage(
    const char *stage,
    unsigned codepoint,
    int glyphIndex,
    const Bitmap<float, 4> &bitmap) {
    if (!smgShouldLogCodepoint(codepoint))
        return;

    const int width = bitmap.width();
    const int height = bitmap.height();
    const int channels = 4;
    if (width <= 0 || height <= 0)
        return;
    const BitmapConstRef<float, 4> bitmapRef(bitmap);

    double minValues[channels];
    double maxValues[channels];
    double sumValues[channels];
    for (int c = 0; c < channels; ++c) {
        minValues[c] = INFINITY;
        maxValues[c] = -INFINITY;
        sumValues[c] = 0;
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float *pixel = bitmap(x, y);
            for (int c = 0; c < channels; ++c) {
                const double value = pixel[c];
                if (value < minValues[c])
                    minValues[c] = value;
                if (value > maxValues[c])
                    maxValues[c] = value;
                sumValues[c] += value;
            }
        }
    }

    const float *rawData = bitmapRef.pixels;
    std::string line = smgCreatePrefix("bitmap", stage, codepoint, glyphIndex);
    line += "|width="+std::to_string(width);
    line += "|height="+std::to_string(height);
    line += "|channels=4";
    line += "|y_axis="+smgFormatYAxis(bitmapRef.yOrientation);
    line += "|min="+smgFormatDouble(minValues[0])+","+smgFormatDouble(minValues[1])+","+smgFormatDouble(minValues[2])+","+smgFormatDouble(minValues[3]);
    line += "|max="+smgFormatDouble(maxValues[0])+","+smgFormatDouble(maxValues[1])+","+smgFormatDouble(maxValues[2])+","+smgFormatDouble(maxValues[3]);
    const double pixelCount = double(width*height);
    line += "|mean="+smgFormatDouble(sumValues[0]/pixelCount)+","+smgFormatDouble(sumValues[1]/pixelCount)+","+smgFormatDouble(sumValues[2]/pixelCount)+","+smgFormatDouble(sumValues[3]/pixelCount);
    line += "|bitmap_hash="+smgComputeFloatHash(rawData, size_t(width)*size_t(height)*4u);
    smgAppendCompareLine(line);

    const int maxX = width-1;
    const int maxY = height-1;
    const int samples[7][2] = {
        {0, 0},
        {width/2, height/2},
        {maxX, maxY},
        {width/4, height/4},
        {(maxX*3)/4, (maxY*3)/4},
        {maxX, 0},
        {0, maxY}
    };

    for (int sampleIndex = 0; sampleIndex < 7; ++sampleIndex) {
        const int x = samples[sampleIndex][0];
        const int y = samples[sampleIndex][1];
        const float *pixel = bitmap(x, y);
        std::string sampleLine = smgCreatePrefix("sample", stage, codepoint, glyphIndex);
        sampleLine += "|sample="+std::to_string(sampleIndex);
        sampleLine += "|x="+std::to_string(x);
        sampleLine += "|y="+std::to_string(y);
        sampleLine += "|values="
            +smgFormatDouble(pixel[0])+","+smgFormatDouble(pixel[1])+","+smgFormatDouble(pixel[2])+","+smgFormatDouble(pixel[3]);
        smgAppendCompareLine(sampleLine);
    }
}

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
            smgLogRequest(*request, int(glyphIndex.getIndex()));

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
            smgLogShapeStage("S01.LoadGlyph", request->codepoint, int(glyphIndex.getIndex()), shape);

            if (!shape.validate()) {
                status = smgFail(SMG_STATUS_SHAPE_ERROR, "Loaded glyph shape is invalid.");
                break;
            }

            shape.orientContours();
            smgLogShapeStage("S02.OrientContours", request->codepoint, int(glyphIndex.getIndex()), shape);
            shape.normalize();
            smgLogShapeStage("S03.Normalize", request->codepoint, int(glyphIndex.getIndex()), shape);
            smgApplyColoring(shape, *request);
            smgLogShapeStage("S04.EdgeColoring", request->codepoint, int(glyphIndex.getIndex()), shape);

            const Shape::Bounds bounds = shape.getBounds();
            const double border = request->pixel_range / request->glyph_scale;
            const double scale = request->glyph_scale;
            const int width = static_cast<int>(std::ceil((bounds.r-bounds.l+2.0*border)*scale));
            const int height = static_cast<int>(std::ceil((bounds.t-bounds.b+2.0*border)*scale));
            smgLogProjectionStage(
                "S05.BoundsProjection",
                request->codepoint,
                int(glyphIndex.getIndex()),
                bounds,
                border,
                scale,
                Vector2(border-bounds.l, border-bounds.b),
                request->pixel_range/request->glyph_scale,
                width,
                height,
                advance);

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
            smgLogBitmapStage("S06.GenerateMTSDF", request->codepoint, int(glyphIndex.getIndex()), generated);

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
