#pragma once
#include "pdf.h"
#include "image_codecs.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <chrono>

namespace pdf {
template <typename T> void release(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}
void hr(HRESULT result, std::string_view operation);
struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};
struct ColorSpace {
    std::string name = "DeviceGray";
    int components = 1;
    Value definition, function;
    std::shared_ptr<ColorSpace> alternate;
    Bytes lookup;
};
struct CodeRange {
    uint32_t low = 0, high = 0;
    uint8_t bytes = 1;
};
struct CMap {
    std::vector<CodeRange> ranges;
    std::unordered_map<uint32_t, std::wstring> unicode;
    std::unordered_map<uint32_t, uint32_t> cid;
    bool identity = false, vertical = false;
};
struct Type1Program {
    std::unordered_map<std::string, Bytes> type1_chars;
    std::vector<Bytes> type1_subrs;
    std::array<std::string, 256> glyph_names;
};
struct Font {
    Value dictionary, descendant;
    IDWriteFontFace* face = nullptr;
    Bytes program;
    std::string base_name;
    CMap cmap, to_unicode;
    std::shared_ptr<CMap> cid_unicode;
    std::array<uint32_t, 256> encoding{};
    std::array<std::string, 256> glyph_names;
    std::unordered_map<uint32_t, double> widths;
    std::unordered_map<uint32_t, std::array<double, 3>> vertical_metrics;
    std::unordered_map<uint32_t, uint16_t> cff_cids;
    std::unordered_map<std::string, uint16_t> cff_names;
    std::unordered_map<uint16_t, uint32_t> glyph_to_unicode;
    std::shared_ptr<Type1Program> type1_program;
    Bytes cid_to_gid;
    double default_width = 0, default_vertical = -1000, default_vorigin = 880;
    Matrix matrix{0.001, 0, 0, 0.001, 0, 0};
    int ascent = 800, descent = -200;
    bool composite = false, type3 = false, type1 = false, embedded = false, symbolic = false;
};
struct Glyph {
    uint32_t code = 0, cid = 0;
    uint16_t index = 0;
    double advance = 0, vadvance = -1000, vx = 0, vy = 880;
    std::wstring text;
    std::string name;
};
struct Segment {
    uint8_t type = 0;
    Point p1, p2, p3;
};
struct Image {
    unsigned width = 0, height = 0;
    Bytes pixels;
    bool stencil = false;
    std::shared_ptr<Image> half;
    unsigned reduction = 0;
};
struct CachedImage {
    std::shared_ptr<Image> image;
    size_t cost = 0;
    uint64_t used = 0;
};
struct TextSpan {
    std::wstring text;
    Rect bounds;
};
struct Link {
    Rect bounds;
    int page = -1;
    std::wstring uri;
};
struct RenderedPage {
    unsigned width = 0, height = 0;
    Bytes pixels;
    std::vector<TextSpan> text;
    std::vector<Link> links;
    std::vector<std::string> warnings;
    double milliseconds = 0;
    Matrix page_matrix;
};
struct ImageProfile {
    uint32_t object = 0;
    unsigned width = 0, height = 0;
    size_t bytes = 0;
    std::string filter;
    unsigned decoded_width = 0, decoded_height = 0, reduction = 0;
};
struct RenderProfile {
    double surface_ms = 0, content_ms = 0, interpret_ms = 0, font_load_ms = 0,
           image_load_ms = 0, image_stream_ms = 0, image_codec_ms = 0, image_convert_ms = 0,
           image_mask_ms = 0, image_resample_ms = 0, image_upload_ms = 0, composite_ms = 0, finish_ms = 0,
           readback_ms = 0, parse_ms = 0, path_ms = 0, shading_ms = 0, form_decode_ms = 0;
    size_t image_hits = 0, image_misses = 0;
    JpxProfile jpx;
    std::vector<ImageProfile> images;
};
// Timers are inactive during normal reading; --profile enables them in pdfprobe.
struct ProfileTimer {
    double* elapsed = nullptr;
    std::chrono::steady_clock::time_point begin;
    explicit ProfileTimer(double* output) : elapsed(output) {
        if (elapsed)
            begin = std::chrono::steady_clock::now();
    }
    void stop() {
        if (elapsed) {
            *elapsed +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            elapsed = nullptr;
        }
    }
    ~ProfileTimer() {
        stop();
    }
    ProfileTimer(const ProfileTimer&) = delete;
    ProfileTimer& operator=(const ProfileTimer&) = delete;
};
struct GradientRamp {
    double low = 0, high = 1;
    int space = 0;
    std::array<Color, 1025> colors{};
    const Value* definition = nullptr;
    const Value* resources = nullptr;
    const Value* tint = nullptr;
};
struct Renderer {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory5* write = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    IWICImagingFactory* wic = nullptr;
    std::unordered_map<const Value*, std::unique_ptr<Font>> fonts;
    std::unordered_map<const Value*, std::shared_ptr<Type1Program>> type1_programs;
    std::unordered_map<const Value*, std::shared_ptr<Bytes>> type3_contents;
    size_t type3_content_bytes = 0;
    std::unordered_map<const Value*, CachedImage> images;
    std::unordered_map<std::string, std::shared_ptr<Image>> inline_stencils;
    size_t inline_stencil_bytes = 0;
    std::unordered_map<std::string, std::shared_ptr<CMap>> character_maps;
    std::unordered_map<const Value*, GradientRamp> gradient_ramps;
    size_t image_bytes = 0;
    size_t image_cache_limit = 128 * 1024 * 1024;
    uint64_t image_clock = 0;
    bool profiling = false;
    unsigned image_workers = 0;
    bool full_resolution_images = false;
    RenderProfile profile;
};
void init_renderer(Renderer& renderer);
void clear_resources(Renderer& renderer);
void close_renderer(Renderer& renderer);
void parse_cmap(View bytes, CMap& cmap, bool unicode);
bool builtin_cmap(std::string_view name, CMap& cmap, bool unicode);
uint32_t adobe_glyph_unicode(std::string_view name);
uint32_t glyph_unicode(std::string_view name);
Font& load_font(Renderer& renderer, Document& doc, const Value& font);
std::vector<Glyph> decode_text(Font& font, std::string_view text);
std::vector<Segment> type1_outline(Font& font, std::string_view name);
ColorSpace color_space(Document& doc, const Value& value, const Value& resources, int depth = 0);
Color color(Document& doc, const ColorSpace& space, std::span<const double> components, int depth = 0);
std::shared_ptr<Image> load_image(Renderer& renderer, Document& doc, const Value& value,
                                  const Value& resources, int depth = 0, double target_width = 0,
                                  double target_height = 0);
std::shared_ptr<Image> image_level(Document& doc, std::shared_ptr<Image> source, double width, double height);
RenderedPage render_page(Renderer& renderer, Document& doc, size_t page, double scale = 1.0, int rotation = 0,
                         bool text_only = false);
void save_png(Renderer& renderer, const RenderedPage& page, const std::filesystem::path& path);
std::wstring page_text(const RenderedPage& page);
} // namespace pdf
