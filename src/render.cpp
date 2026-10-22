#include "render.h"
#include <chrono>
#include <charconv>
#include <functional>
#include <numbers>

namespace pdf {
void hr(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        char code[32]{};
        snprintf(code, sizeof(code), " (0x%08lX)", static_cast<unsigned long>(result));
        fail(std::string(operation) + code);
    }
}
void init_renderer(Renderer& r) {
    try {
        hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &r.d2d), "Create Direct2D factory");
        hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5),
                               reinterpret_cast<IUnknown**>(&r.write)),
           "Create DirectWrite factory (Windows 10 1703 or later required)");
        hr(r.write->CreateInMemoryFontFileLoader(&r.loader), "Create embedded font loader");
        hr(r.write->RegisterFontFileLoader(r.loader), "Register embedded font loader");
        hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&r.wic)),
           "Create Windows image codecs");
    } catch (...) {
        close_renderer(r);
        throw;
    }
}
void clear_resources(Renderer& r) {
    for (auto& pair : r.fonts)
        release(pair.second->face);
    r.fonts.clear();
    r.type1_programs.clear();
    r.type3_contents.clear();
    r.type3_content_bytes = 0;
    r.images.clear();
    r.inline_stencils.clear();
    r.inline_stencil_bytes = 0;
    r.character_maps.clear();
    r.gradient_ramps.clear();
    r.image_bytes = 0;
    r.image_clock = 0;
    if (r.loader && r.write) {
        r.write->UnregisterFontFileLoader(r.loader);
        release(r.loader);
        hr(r.write->CreateInMemoryFontFileLoader(&r.loader), "Reset font loader");
        hr(r.write->RegisterFontFileLoader(r.loader), "Register font loader");
    }
}
void close_renderer(Renderer& r) {
    for (auto& pair : r.fonts)
        release(pair.second->face);
    r.fonts.clear();
    r.type1_programs.clear();
    r.type3_contents.clear();
    r.type3_content_bytes = 0;
    r.images.clear();
    r.inline_stencils.clear();
    r.inline_stencil_bytes = 0;
    r.character_maps.clear();
    r.gradient_ramps.clear();
    if (r.loader && r.write)
        r.write->UnregisterFontFileLoader(r.loader);
    release(r.loader);
    release(r.write);
    release(r.d2d);
    release(r.wic);
}
static D2D1_MATRIX_3X2_F matrix(Matrix m) {
    return {float(m.a), float(m.b), float(m.c), float(m.d), float(m.e), float(m.f)};
}
static D2D1_POINT_2F point(Point p) {
    return {float(p.x), float(p.y)};
}
static D2D1_COLOR_F rgba(Color c, float alpha = 1) {
    return {c.r, c.g, c.b, c.a * alpha};
}
struct StencilBitmap {
    std::shared_ptr<Image> source;
    Color color;
    ID2D1Bitmap* bitmap = nullptr;
    ID2D1BitmapBrush* edge_brush = nullptr;
};
struct Surface {
    IWICBitmap* bitmap = nullptr;
    ID2D1RenderTarget* target = nullptr;
    ID2D1SolidColorBrush* solid_brush = nullptr;
    ID2D1StrokeStyle* stroke_style = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES stroke_properties{};
    std::vector<float> stroke_dashes;
    std::unordered_map<const Image*, StencilBitmap> stencils;
    size_t stencil_bytes = 0;
    unsigned width = 0, height = 0;
};
static void destroy_surface(Surface* s) {
    if (s) {
        for (auto& [key, entry] : s->stencils) {
            release(entry.edge_brush);
            release(entry.bitmap);
        }
        release(s->solid_brush);
        release(s->stroke_style);
        release(s->target);
        release(s->bitmap);
        delete s;
    }
}
using SurfacePtr = std::unique_ptr<Surface, decltype(&destroy_surface)>;
static SurfacePtr surface(Renderer& r, unsigned w, unsigned h) {
    SurfacePtr s(new Surface{}, destroy_surface);
    s->width = w;
    s->height = h;
    hr(r.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &s->bitmap),
       "Allocate page bitmap");
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    hr(r.d2d->CreateWicBitmapRenderTarget(s->bitmap, props, &s->target), "Create page render target");
    s->target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return s;
}
struct Graphics {
    Matrix ctm, text, line;
    Color fill, stroke;
    ColorSpace fill_space, stroke_space;
    std::vector<double> fill_components{0}, stroke_components{0};
    std::string fill_pattern, stroke_pattern, blend = "Normal";
    double width = 1, miter = 10, dash_offset = 0;
    int cap = 0, join = 0;
    std::vector<float> dash;
    float fill_alpha = 1, stroke_alpha = 1;
    Font* font = nullptr;
    double font_size = 12, char_space = 0, word_space = 0, hscale = 1, leading = 0, rise = 0;
    int text_mode = 0;
    size_t clips = 0;
    Value softmask;
    Matrix mask_matrix;
};
struct Clip {
    ID2D1Geometry* geometry = nullptr;
    D2D1_RECT_F bounds{};
};
struct Context {
    Renderer* renderer = nullptr;
    Document* document = nullptr;
    Surface* canvas = nullptr;
    RenderedPage* output = nullptr;
    Graphics g;
    std::vector<Clip> clips;
    size_t operations = 0;
    int depth = 0;
    bool collect_text = true;
    bool alpha_only = false;
    bool text_only = false;
    bool glyph_image = false;
    int origin_x = 0, origin_y = 0;
};
static void set_transform(Context& c, Matrix m = {}) {
    c.canvas->target->SetTransform(
        matrix(multiply({1, 0, 0, 1, -double(c.origin_x), -double(c.origin_y)}, m)));
}
static void interpret(Context& c, View bytes, const Value& resources, int depth);
static void draw_form(Context& c, const Value& form, const Value& resources, int depth, bool group = true);
static void apply_clip(Context& c, const Clip& clip,
                       D2D1_ANTIALIAS_MODE antialias = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE) {
    set_transform(c);
    if (clip.geometry)
        c.canvas->target->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), clip.geometry, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::Matrix3x2F::Identity(), 1), nullptr);
    else
        c.canvas->target->PushAxisAlignedClip(clip.bounds, antialias);
}
static void set_rect_clip_antialias(Context& c, D2D1_ANTIALIAS_MODE antialias) {
    // Change only the rectangular clips above the last geometry layer. Popping
    // a geometry layer here would composite earlier drawing into its parent.
    size_t first = c.clips.size();
    while (first && !c.clips[first - 1].geometry)
        --first;
    if (first == c.clips.size())
        return;
    D2D1_MATRIX_3X2_F transform;
    c.canvas->target->GetTransform(&transform);
    for (size_t i = c.clips.size(); i > first; --i)
        c.canvas->target->PopAxisAlignedClip();
    for (size_t i = first; i < c.clips.size(); ++i)
        apply_clip(c, c.clips[i], antialias);
    c.canvas->target->SetTransform(transform);
}
static bool rect_clip_affects_edges(const Context& c, Rect bounds) {
    if (!std::isfinite(bounds.x0) || !std::isfinite(bounds.y0) ||
        !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1))
        return true;
    // These are the same clips changed by set_rect_clip_antialias. An image
    // at least one pixel inside each edge cannot touch their partial coverage.
    for (auto clip = c.clips.rbegin(); clip != c.clips.rend() && !clip->geometry; ++clip)
        if (bounds.x0 < clip->bounds.left + 1 || bounds.y0 < clip->bounds.top + 1 ||
            bounds.x1 > clip->bounds.right - 1 || bounds.y1 > clip->bounds.bottom - 1)
            return true;
    return false;
}
static void push_clip(Context& c, Clip clip) {
    require(c.clips.size() < 256, "Graphics clipping depth limit exceeded");
    c.clips.push_back(clip);
    if (clip.geometry)
        clip.geometry->AddRef();
    apply_clip(c, clip);
    c.g.clips = c.clips.size();
}
static void push_clip(Context& c, ID2D1Geometry* geometry) {
    Clip clip{geometry};
    hr(geometry->GetBounds(nullptr, &clip.bounds), "Measure clipping bounds");
    push_clip(c, clip);
}
static void remove_clip(Context& c, const Clip& clip) {
    if (clip.geometry)
        c.canvas->target->PopLayer();
    else
        c.canvas->target->PopAxisAlignedClip();
}
static void pop_clips(Context& c, size_t count) {
    while (c.clips.size() > count) {
        auto clip = c.clips.back();
        remove_clip(c, clip);
        release(clip.geometry);
        c.clips.pop_back();
    }
    c.g.clips = c.clips.size();
}
static void suspend(Context& c) {
    for (auto it = c.clips.rbegin(); it != c.clips.rend(); ++it)
        remove_clip(c, *it);
    hr(c.canvas->target->EndDraw(), "Finish drawing for composition");
}
static void resume(Context& c) {
    c.canvas->target->BeginDraw();
    set_transform(c);
    for (const auto& clip : c.clips)
        apply_clip(c, clip);
}
static bool axis_rectangle(const std::vector<Segment>& path, D2D1_RECT_F& bounds) {
    if (path.size() < 4 || path.size() > 6 || path[0].type != 0 || path[1].type != 1 ||
        path[2].type != 1 || path[3].type != 1)
        return false;
    for (size_t i = 4; i < path.size(); ++i)
        if (path[i].type != 3 && (path[i].type != 1 || path[i].p1.x != path[0].p1.x ||
                                 path[i].p1.y != path[0].p1.y))
            return false;
    const auto a = path[0].p1, b = path[1].p1, c = path[2].p1, d = path[3].p1;
    if (!((a.y == b.y && b.x == c.x && c.y == d.y && d.x == a.x) ||
          (a.x == b.x && b.y == c.y && c.x == d.x && d.y == a.y)))
        return false;
    bounds = {float(std::min(a.x, c.x)), float(std::min(a.y, c.y)),
              float(std::max(a.x, c.x)), float(std::max(a.y, c.y))};
    return true;
}
static ID2D1PathGeometry* geometry(Context& c, const std::vector<Segment>& path, bool evenodd = false,
                                  bool stroked = false) {
    ID2D1PathGeometry* shape = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    hr(c.renderer->d2d->CreatePathGeometry(&shape), "Create vector path");
    try {
        hr(shape->Open(&sink), "Open vector path");
        sink->SetFillMode(evenodd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
        bool open = false;
        for (size_t i = 0; i < path.size(); ++i) {
            auto& seg = path[i];
            if (seg.type == 0) {
                if (open)
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                open = false;
                if (stroked) {
                    size_t end = i + 1;
                    bool dot = true;
                    auto same = [&](Point p) { return p.x == seg.p1.x && p.y == seg.p1.y; };
                    while (end < path.size() && path[end].type != 0) {
                        auto& s = path[end++];
                        if ((s.type == 1 && !same(s.p1)) ||
                            (s.type == 2 && (!same(s.p1) || !same(s.p2) || !same(s.p3))))
                            dot = false;
                    }
                    if (dot) {
                        // A degenerate subpath has no cap direction. PDF paints
                        // it only with round caps; a trailing move paints nothing.
                        if (c.g.cap == 1 && end > i + 1) {
                            sink->BeginFigure(point(seg.p1), D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(point(seg.p1));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        }
                        i = end - 1;
                        continue;
                    }
                }
                sink->BeginFigure(point(seg.p1), D2D1_FIGURE_BEGIN_FILLED);
                open = true;
            } else if (seg.type == 1) {
                if (!open) {
                    sink->BeginFigure(point(seg.p1), D2D1_FIGURE_BEGIN_FILLED);
                    open = true;
                } else
                    sink->AddLine(point(seg.p1));
            } else if (seg.type == 2) {
                if (open)
                    sink->AddBezier({point(seg.p1), point(seg.p2), point(seg.p3)});
            } else if (seg.type == 3 && open) {
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
            }
        }
        if (open)
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        hr(sink->Close(), "Close vector path");
        release(sink);
    } catch (...) {
        release(sink);
        release(shape);
        throw;
    }
    return shape;
}
static void clip_rect(Context& c, Rect r, Matrix m) {
    std::vector<Segment> path{{0, transform(m, {r.x0, r.y0}), {}, {}},
                              {1, transform(m, {r.x1, r.y0}), {}, {}},
                              {1, transform(m, {r.x1, r.y1}), {}, {}},
                              {1, transform(m, {r.x0, r.y1}), {}, {}},
                              {3, {}, {}, {}}};
    D2D1_RECT_F bounds{};
    if (axis_rectangle(path, bounds)) {
        push_clip(c, Clip{nullptr, bounds});
        return;
    }
    auto shape = geometry(c, path);
    push_clip(c, shape);
    release(shape);
}
static std::array<double, 3> blend_rgb(std::string_view mode, std::array<double, 3> b,
                                       std::array<double, 3> s) {
    auto lum = [](auto v) { return 0.3 * v[0] + 0.59 * v[1] + 0.11 * v[2]; };
    auto sat = [](auto v) {
        return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
    };
    auto set_lum = [&](std::array<double, 3> v, double l) {
        double delta = l - lum(v);
        for (auto& x : v)
            x += delta;
        double lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
        if (lo < 0)
            for (auto& x : v)
                x = l + (x - l) * l / (l - lo);
        if (hi > 1)
            for (auto& x : v)
                x = l + (x - l) * (1 - l) / (hi - l);
        return v;
    };
    auto set_sat = [&](std::array<double, 3> v, double saturation) {
        double lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
        for (auto& x : v)
            x = hi > lo ? (x - lo) * saturation / (hi - lo) : 0;
        return v;
    };
    if (mode == "Hue")
        return set_lum(set_sat(s, sat(b)), lum(b));
    if (mode == "Saturation")
        return set_lum(set_sat(b, sat(s)), lum(b));
    if (mode == "Color")
        return set_lum(s, lum(b));
    if (mode == "Luminosity")
        return set_lum(b, lum(s));
    for (int i = 0; i < 3; ++i) {
        double cb = b[i], cs = s[i], out = cs;
        if (mode == "Multiply")
            out = cb * cs;
        else if (mode == "Screen")
            out = cb + cs - cb * cs;
        else if (mode == "Overlay")
            out = cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
        else if (mode == "Darken")
            out = std::min(cb, cs);
        else if (mode == "Lighten")
            out = std::max(cb, cs);
        else if (mode == "ColorDodge")
            out = cb == 0 ? 0 : cs == 1 ? 1 : std::min(1.0, cb / (1 - cs));
        else if (mode == "ColorBurn")
            out = cb == 1 ? 1 : cs == 0 ? 0 : 1 - std::min(1.0, (1 - cb) / cs);
        else if (mode == "HardLight")
            out = cs <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
        else if (mode == "SoftLight") {
            double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
            out = cs <= 0.5 ? cb - (1 - 2 * cs) * cb * (1 - cb) : cb + (2 * cs - 1) * (d - cb);
        } else if (mode == "Difference")
            out = std::abs(cb - cs);
        else if (mode == "Exclusion")
            out = cb + cs - 2 * cb * cs;
        s[i] = out;
    }
    return s;
}
static Bytes pixels(Surface& s) {
    Bytes bytes(size_t(s.width) * s.height * 4);
    hr(s.bitmap->CopyPixels(nullptr, s.width * 4, UINT(bytes.size()), bytes.data()), "Read rendered pixels");
    return bytes;
}
static void copy_backdrop(Context& c, Surface& layer, int origin_x, int origin_y) {
    WICRect rect{origin_x - c.origin_x, origin_y - c.origin_y, int(layer.width), int(layer.height)};
    IWICBitmapLock* lock = nullptr;
    try {
        hr(layer.bitmap->Lock(nullptr, WICBitmapLockWrite, &lock), "Initialize transparency group backdrop");
        UINT stride = 0, size = 0;
        BYTE* destination = nullptr;
        hr(lock->GetStride(&stride), "Read backdrop stride");
        hr(lock->GetDataPointer(&size, &destination), "Read backdrop buffer");
        hr(c.canvas->bitmap->CopyPixels(&rect, stride, size, destination),
           "Copy transparency group backdrop");
        release(lock);
    } catch (...) {
        release(lock);
        throw;
    }
}
static Bytes soft_mask(Context& c, const Value& resources) {
    auto& mask = resolve(*c.document, c.g.softmask);
    auto& form = entry(*c.document, mask, "G");
    require(form.has_stream, "Invalid soft-mask group");
    auto layer = surface(*c.renderer, c.canvas->width, c.canvas->height);
    Context sub;
    sub.renderer = c.renderer;
    sub.document = c.document;
    sub.canvas = layer.get();
    sub.output = c.output;
    sub.g.ctm = c.g.mask_matrix;
    sub.collect_text = false;
    sub.depth = c.depth + 1;
    sub.origin_x = c.origin_x;
    sub.origin_y = c.origin_y;
    layer->target->BeginDraw();
    layer->target->Clear(D2D1::ColorF(0, 0));
    try {
        auto& background = entry(*c.document, mask, "BC");
        if (background.kind == Kind::array) {
            auto space = color_space(*c.document, get(entry(*c.document, form, "Group"), "CS"), resources);
            std::vector<double> values;
            for (auto& v : background.array)
                values.push_back(num(v));
            layer->target->Clear(rgba(color(*c.document, space, values)));
        }
        draw_form(sub, form, resources, sub.depth, false);
        pop_clips(sub, 0);
        hr(layer->target->EndDraw(), "Render soft mask");
    } catch (...) {
        pop_clips(sub, 0);
        layer->target->EndDraw();
        throw;
    }
    Bytes data = pixels(*layer);
    Bytes alpha(size_t(layer->width) * layer->height);
    bool luminosity = named(get(mask, "S"), "Luminosity");
    std::array<uint8_t, 256> transfer{};
    auto& fn = entry(*c.document, mask, "TR");
    for (int i = 0; i < 256; ++i) {
        double x = i / 255.0;
        if (fn.kind == Kind::dict) {
            auto result = evaluate_function(*c.document, fn, std::span(&x, 1));
            if (!result.empty())
                x = result[0];
        }
        transfer[i] = uint8_t(std::clamp(x, 0.0, 1.0) * 255 + 0.5);
    }
    for (size_t i = 0; i < alpha.size(); ++i) {
        unsigned v = luminosity
                         ? unsigned(0.3 * data[4 * i + 2] + 0.59 * data[4 * i + 1] + 0.11 * data[4 * i])
                         : data[4 * i + 3];
        alpha[i] = transfer[std::min(v, 255u)];
    }
    return alpha;
}
static void composite(Context& c, Surface& layer, std::string_view mode, float opacity, const Bytes& mask,
                      int origin_x, int origin_y, bool has_backdrop = false, View group_alpha = {}) {
    ProfileTimer timer(c.renderer->profiling ? &c.renderer->profile.composite_ms : nullptr);
    Bytes source = pixels(layer);
    // A backdrop group keeps its parent paused from the initial backdrop copy
    // until composition finishes, avoiding a second flush of every clip layer.
    if (!has_backdrop)
        suspend(c);
    IWICBitmapLock* lock = nullptr;
    try {
        WICRect rect{0, 0, int(c.canvas->width), int(c.canvas->height)};
        hr(c.canvas->bitmap->Lock(&rect, WICBitmapLockWrite, &lock), "Lock composition target");
        UINT stride = 0, size = 0;
        BYTE* dest = nullptr;
        hr(lock->GetStride(&stride), "Get composition stride");
        hr(lock->GetDataPointer(&size, &dest), "Get composition buffer");
        for (unsigned y = 0; y < layer.height; ++y) {
            if ((y & 127) == 0)
                check_cancel(*c.document);
            for (unsigned x = 0; x < layer.width; ++x) {
                size_t i = (size_t(y) * layer.width + x) * 4;
                BYTE* dst = dest + size_t(int(y) + origin_y - c.origin_y) * stride +
                            size_t(int(x) + origin_x - c.origin_x) * 4;
                double coverage = opacity * (mask.empty() ? 1 : mask[i / 4] / 255.0);
                if (has_backdrop && group_alpha.empty()) {
                    if (coverage == 0 || std::memcmp(dst, source.data() + i, 4) == 0)
                        continue;
                    // For normal group composition, removing the initial backdrop
                    // and compositing the group again reduces to this interpolation
                    // in premultiplied color/alpha, even with a translucent backdrop.
                    for (int k = 0; k < 4; ++k)
                        dst[k] = uint8_t(
                            std::clamp(dst[k] + coverage * (source[i + k] - dst[k]), 0.0, 255.0) + 0.5);
                    continue;
                }
                double source_alpha = group_alpha.empty() ? source[i + 3] / 255.0 : group_alpha[i / 4] / 255.0;
                double sa = source_alpha * coverage, da = dst[3] / 255.0;
                if (sa == 0)
                    continue;
                std::array<double, 3> sc{}, dc{};
                for (int k = 0; k < 3; ++k) {
                    sc[k] = source[i + 2 - k] / 255.0;
                    dc[k] = dst[2 - k] / 255.0;
                    if (has_backdrop)
                        sc[k] = std::clamp(sc[k] - (1 - source_alpha) * dc[k], 0.0, source_alpha);
                    sc[k] *= coverage;
                }
                std::array<double, 3> su{}, du{};
                for (int k = 0; k < 3; ++k) {
                    su[k] = sc[k] / sa;
                    du[k] = da ? dc[k] / da : 0;
                }
                auto blended = blend_rgb(mode, du, su);
                for (int k = 0; k < 3; ++k)
                    dst[2 - k] = uint8_t(
                        std::clamp((1 - sa) * dc[k] + (1 - da) * sc[k] + sa * da * blended[k], 0.0, 1.0) *
                            255 +
                        0.5);
                dst[3] = uint8_t(std::clamp(sa + da - sa * da, 0.0, 1.0) * 255 + 0.5);
            }
        }
        release(lock);
        if (!has_backdrop)
            resume(c);
    } catch (...) {
        release(lock);
        if (!has_backdrop)
            resume(c);
        throw;
    }
}
static Bytes group_alpha(Context& c, const std::function<void(Context&)>& draw,
                         int x, int y, unsigned width, unsigned height) {
    auto layer = surface(*c.renderer, width, height);
    Context sub;
    sub.renderer = c.renderer;
    sub.document = c.document;
    sub.canvas = layer.get();
    sub.output = c.output;
    sub.g = c.g;
    sub.g.blend = "Normal";
    sub.g.softmask = {};
    sub.depth = c.depth + 1;
    sub.collect_text = false;
    sub.alpha_only = true;
    sub.origin_x = x;
    sub.origin_y = y;
    layer->target->BeginDraw();
    layer->target->Clear(D2D1::ColorF(0, 0));
    try {
        for (auto clip : c.clips)
            push_clip(sub, clip);
        draw(sub);
        pop_clips(sub, 0);
        hr(layer->target->EndDraw(), "Render group alpha");
    } catch (...) {
        pop_clips(sub, 0);
        layer->target->EndDraw();
        throw;
    }
    auto data = pixels(*layer);
    Bytes alpha(size_t(width) * height);
    for (size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = data[i * 4 + 3];
    return alpha;
}
static void paint(Context& c, const Value& resources, const std::function<void(Context&)>& draw,
                  bool force = false, float opacity = 1, Rect bounds = {-1e30, -1e30, 1e30, 1e30},
                  bool has_backdrop = false, bool image_mask = false) {
    bool use_mask = !image_mask && c.g.softmask.kind != Kind::null;
    bool normal = c.alpha_only || c.g.blend == "Normal" || c.g.blend == "Compatible";
    has_backdrop = has_backdrop && !c.alpha_only;
    if (!force && normal && !use_mask) {
        draw(c);
        return;
    }
    require(c.depth < 32, "Transparency group recursion limit");
    bounds.x0 = std::max(bounds.x0, double(c.origin_x));
    bounds.y0 = std::max(bounds.y0, double(c.origin_y));
    bounds.x1 = std::min(bounds.x1, double(c.origin_x) + c.canvas->width);
    bounds.y1 = std::min(bounds.y1, double(c.origin_y) + c.canvas->height);
    for (auto clip : c.clips) {
        auto box = clip.bounds;
        bounds.x0 = std::max(bounds.x0, double(box.left) - 1);
        bounds.y0 = std::max(bounds.y0, double(box.top) - 1);
        bounds.x1 = std::min(bounds.x1, double(box.right) + 1);
        bounds.y1 = std::min(bounds.y1, double(box.bottom) + 1);
    }
    if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0)
        return;
    int x = int(std::floor(bounds.x0)), y = int(std::floor(bounds.y0));
    unsigned width = unsigned(std::ceil(bounds.x1) - x), height = unsigned(std::ceil(bounds.y1) - y);
    auto layer = surface(*c.renderer, width, height);
    Context sub;
    sub.renderer = c.renderer;
    sub.document = c.document;
    sub.canvas = layer.get();
    sub.output = c.output;
    sub.g = c.g;
    sub.g.blend = "Normal";
    sub.g.softmask = {};
    sub.depth = c.depth + 1;
    sub.collect_text = c.collect_text;
    sub.alpha_only = c.alpha_only;
    sub.origin_x = x;
    sub.origin_y = y;
    if (has_backdrop)
        suspend(c);
    bool drawing = false;
    try {
        if (has_backdrop)
            copy_backdrop(c, *layer, x, y);
        layer->target->BeginDraw();
        drawing = true;
        if (!has_backdrop)
            layer->target->Clear(D2D1::ColorF(0, 0));
        for (auto clip : c.clips)
            push_clip(sub, clip);
        draw(sub);
        pop_clips(sub, 0);
        HRESULT result = layer->target->EndDraw();
        drawing = false;
        hr(result, "Render transparency layer");
        Bytes mask;
        if (use_mask) {
            sub.g = c.g;
            mask = soft_mask(sub, resources);
        }
        // A non-normal group blend needs the group's alpha without its initial
        // backdrop, then explicit backdrop removal (PDF 32000-1, 11.4.4).
        // The alpha pass ignores blend colors and does not start nested alpha
        // passes; soft-mask luminosity calculations still use normal color rendering.
        Bytes alpha;
        if (has_backdrop && !normal)
            alpha = group_alpha(c, draw, x, y, width, height);
        composite(c, *layer, c.alpha_only ? "Normal" : c.g.blend, opacity, mask, x, y, has_backdrop, alpha);
        if (has_backdrop)
            resume(c);
    } catch (...) {
        if (drawing) {
            pop_clips(sub, 0);
            layer->target->EndDraw();
        }
        if (has_backdrop)
            resume(c);
        throw;
    }
}
static Matrix value_matrix(const Value& v) {
    if (v.array.size() != 6)
        return {};
    return {num(at(v, 0)), num(at(v, 1)), num(at(v, 2)), num(at(v, 3)), num(at(v, 4)), num(at(v, 5))};
}
static bool visible(Document& d, const Value& input, int depth = 0) {
    if (depth > 16)
        return true;
    auto& v = resolve(d, input);
    if (v.kind == Kind::null)
        return true;
    if (named(get(v, "Type"), "OCG")) {
        auto& configuration = entry(d, entry(d, d.catalog, "OCProperties"), "D");
        bool on = !named(get(configuration, "BaseState"), "OFF");
        for (auto& off : entry(d, configuration, "OFF").array)
            if (input.kind == Kind::ref && off.object == input.object)
                on = false;
        for (auto& item : entry(d, configuration, "ON").array)
            if (input.kind == Kind::ref && item.object == input.object)
                on = true;
        return on;
    }
    if (named(get(v, "Type"), "OCMD")) {
        auto& ocgs = entry(d, v, "OCGs");
        std::vector<bool> states;
        if (ocgs.kind == Kind::array)
            for (auto& item : ocgs.array)
                states.push_back(visible(d, item, depth + 1));
        else
            states.push_back(visible(d, ocgs, depth + 1));
        auto policy = get(v, "P").text;
        if (policy == "AllOn")
            return std::all_of(states.begin(), states.end(), [](bool x) { return x; });
        if (policy == "AnyOff")
            return std::any_of(states.begin(), states.end(), [](bool x) { return !x; });
        if (policy == "AllOff")
            return std::all_of(states.begin(), states.end(), [](bool x) { return !x; });
        return std::any_of(states.begin(), states.end(), [](bool x) { return x; });
    }
    return true;
}
static ID2D1Brush* make_brush(Context& c, const Value& resources, bool stroke);
static Rect transformed_box(Matrix m, Rect r);
static void draw_path(Context& c, const Value& resources, const std::vector<Segment>& path, bool fill,
                      bool stroke, bool evenodd) {
    ProfileTimer timer(c.renderer->profiling ? &c.renderer->profile.path_ms : nullptr);
    bool line = !fill && stroke && path.size() == 2 && path[0].type == 0 && path[1].type == 1;
    if (line && c.g.cap != 1 && path[0].p1.x == path[1].p1.x && path[0].p1.y == path[1].p1.y)
        return;
    D2D1_RECT_F box_bounds{};
    bool box = fill && !stroke && axis_rectangle(path, box_bounds);
    auto shape = line || box ? nullptr : geometry(c, path, evenodd, stroke && !fill);
    ID2D1PathGeometry* stroke_shape = nullptr;
    try {
        if (fill && stroke)
            stroke_shape = geometry(c, path, evenodd, true);
        Rect bounds{-1e30, -1e30, 1e30, 1e30};
        if ((c.g.blend != "Normal" && c.g.blend != "Compatible") || c.g.softmask.kind != Kind::null) {
            D2D1_RECT_F extent{};
            if (line)
                extent = {float(std::min(path[0].p1.x, path[1].p1.x)), float(std::min(path[0].p1.y, path[1].p1.y)),
                          float(std::max(path[0].p1.x, path[1].p1.x)), float(std::max(path[0].p1.y, path[1].p1.y))};
            else if (box)
                extent = box_bounds;
            else
                hr(shape->GetBounds(nullptr, &extent), "Measure paint bounds");
            double padding = stroke ? std::max(2.0, std::abs(c.g.width) *
                std::max(std::hypot(c.g.ctm.a, c.g.ctm.b), std::hypot(c.g.ctm.c, c.g.ctm.d)) *
                std::max(1.0, c.g.miter)) : 2;
            bounds = {extent.left - padding, extent.top - padding, extent.right + padding, extent.bottom + padding};
        }
        paint(
            c, resources,
            [&](Context& target) {
                auto rt = target.canvas->target;
                set_transform(target);
                ID2D1Brush* brush = nullptr;
                ID2D1StrokeStyle* style = nullptr;
                ID2D1TransformedGeometry* local = nullptr;
                try {
                    if (fill) {
                        brush = make_brush(target, resources, false);
                        if (box)
                            rt->FillRectangle(box_bounds, brush);
                        else
                            rt->FillGeometry(shape, brush);
                        release(brush);
                    }
                    if (stroke) {
                        brush = make_brush(target, resources, true);
                        double scale = std::sqrt(std::max(1e-16, std::abs(target.g.ctm.a * target.g.ctm.d -
                                                                          target.g.ctm.b * target.g.ctm.c)));
                        // Direct2D drops strokes below its coverage precision.
                        // Keep hairlines and very thin plot grids visible at
                        // display resolution, including when the PDF width is 0.
                        float width = float(std::max(0.2 / scale, std::abs(target.g.width)));
                        auto props = D2D1::StrokeStyleProperties();
                        props.startCap = props.endCap = props.dashCap =
                            target.g.cap == 1   ? D2D1_CAP_STYLE_ROUND
                            : target.g.cap == 2 ? D2D1_CAP_STYLE_SQUARE
                                                : D2D1_CAP_STYLE_FLAT;
                        props.lineJoin = target.g.join == 1   ? D2D1_LINE_JOIN_ROUND
                                         : target.g.join == 2 ? D2D1_LINE_JOIN_BEVEL
                                                              : D2D1_LINE_JOIN_MITER;
                        props.miterLimit = float(std::max(1.0, target.g.miter));
                        props.dashStyle =
                            target.g.dash.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM;
                        props.dashOffset = float(target.g.dash_offset / std::max(0.001f, width));
                        std::vector<float> dash = target.g.dash;
                        for (auto& value : dash)
                            value /= std::max(0.001f, width);
                        if (dash.size() % 2) {
                            auto copy = dash;
                            dash.insert(dash.end(), copy.begin(), copy.end());
                        }
                        auto& canvas = *target.canvas;
                        auto& old_props = canvas.stroke_properties;
                        if (!canvas.stroke_style || old_props.startCap != props.startCap ||
                            old_props.endCap != props.endCap || old_props.dashCap != props.dashCap ||
                            old_props.lineJoin != props.lineJoin || old_props.miterLimit != props.miterLimit ||
                            old_props.dashStyle != props.dashStyle || old_props.dashOffset != props.dashOffset ||
                            canvas.stroke_dashes != dash) {
                            auto cached_dash = dash;
                            hr(target.renderer->d2d->CreateStrokeStyle(
                                   props, dash.empty() ? nullptr : dash.data(), UINT32(dash.size()), &style),
                               "Create stroke style");
                            release(canvas.stroke_style);
                            canvas.stroke_style = style;
                            canvas.stroke_properties = props;
                            canvas.stroke_dashes = std::move(cached_dash);
                        } else
                            style = canvas.stroke_style;
                        style->AddRef();
                        Matrix inv = inverse(target.g.ctm);
                        if (!line)
                            hr(target.renderer->d2d->CreateTransformedGeometry(stroke_shape ? stroke_shape : shape,
                                                                              matrix(inv), &local),
                               "Transform stroke geometry");
                        D2D1_MATRIX_3X2_F old;
                        brush->GetTransform(&old);
                        brush->SetTransform(old * matrix(inv));
                        set_transform(target, target.g.ctm);
                        if (line)
                            rt->DrawLine(point(transform(inv, path[0].p1)), point(transform(inv, path[1].p1)),
                                         brush, width, style);
                        else
                            rt->DrawGeometry(local, brush, width, style);
                        release(local);
                        release(style);
                        release(brush);
                    }
                } catch (...) {
                    release(local);
                    release(style);
                    release(brush);
                    throw;
                }
            },
            false, 1, bounds);
    } catch (...) {
        release(stroke_shape);
        release(shape);
        throw;
    }
    release(stroke_shape);
    release(shape);
}
static void draw_image(Context& c, const Value& image, const Value& resources,
                       std::shared_ptr<Image> decoded = {}) {
    double width = std::hypot(c.g.ctm.a, c.g.ctm.b), height = std::hypot(c.g.ctm.c, c.g.ctm.d);
    if (!decoded)
        decoded = load_image(*c.renderer, *c.document, image, resources, 0, width, height);
    ProfileTimer resample_timer(c.renderer->profiling ? &c.renderer->profile.image_resample_ms : nullptr);
    decoded = image_level(*c.document, decoded, width, height);
    resample_timer.stop();
    // An image's own mask replaces the graphics-state soft mask. Blend mode
    // and constant opacity still apply (PDF 32000-1, 11.6.4.3 and Table 89).
    auto& d = *c.document;
    auto& mask = entry(d, image, "Mask");
    bool image_mask = !decoded->stencil && (entry(d, image, "SMask").has_stream ||
                      mask.has_stream || mask.kind == Kind::array || integer(get(image, "SMaskInData")) != 0);
    paint(
        c, resources,
        [&](Context& target) {
            ID2D1Bitmap* bitmap = nullptr;
            auto& canvas = *target.canvas;
            auto cached = canvas.stencils.find(decoded.get());
            if (cached != canvas.stencils.end() && cached->second.color.r == target.g.fill.r &&
                cached->second.color.g == target.g.fill.g && cached->second.color.b == target.g.fill.b) {
                bitmap = cached->second.bitmap;
                bitmap->AddRef();
            }
            Bytes pixels_copy;
            auto* data = decoded->pixels.data();
            if (!bitmap && decoded->stencil) {
                pixels_copy = decoded->pixels;
                for (size_t i = 0; i < pixels_copy.size(); i += 4) {
                    unsigned a = pixels_copy[i + 3];
                    pixels_copy[i] = uint8_t(target.g.fill.b * a);
                    pixels_copy[i + 1] = uint8_t(target.g.fill.g * a);
                    pixels_copy[i + 2] = uint8_t(target.g.fill.r * a);
                }
                data = pixels_copy.data();
            }
            ProfileTimer upload_timer(c.renderer->profiling ? &c.renderer->profile.image_upload_ms : nullptr);
            if (!bitmap)
                hr(target.canvas->target->CreateBitmap(
                   D2D1::SizeU(decoded->width, decoded->height), data, decoded->width * 4,
                   D2D1::BitmapProperties(
                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                   &bitmap),
               "Upload image pixels");
            upload_timer.stop();
            if (decoded->stencil && cached == canvas.stencils.end() && decoded->pixels.size() <= 65536 &&
                canvas.stencils.size() < 4096 && canvas.stencil_bytes + decoded->pixels.size() * 2 < 16 * 1024 * 1024) {
                try {
                    canvas.stencils.emplace(decoded.get(), StencilBitmap{decoded, target.g.fill, bitmap});
                    bitmap->AddRef();
                    canvas.stencil_bytes += decoded->pixels.size() * 2;
                } catch (...) {
                    release(bitmap);
                    throw;
                }
            }
            auto image_transform = multiply(target.g.ctm, {1, 0, 0, -1, 0, 1});
            set_transform(target, image_transform);
            bool interpolate = boolean(get(image, "Interpolate"), true);
            auto antialias = target.canvas->target->GetAntialiasMode();
            // Image sampling still interpolates pixels. Axis-aligned outer
            // edges use pixel centers, so adjacent image tiles meet exactly.
            // Stencil glyphs and subpixel rules need fractional coverage.
            // Expanding a glyph's bitmap boundary makes its stems too heavy.
            bool fractional_stencil = decoded->stencil && (target.glyph_image || width < 1 || height < 1);
            bool aligned = !fractional_stencil &&
                           ((target.g.ctm.b == 0 && target.g.ctm.c == 0) ||
                            (target.g.ctm.a == 0 && target.g.ctm.d == 0));
            ID2D1BitmapBrush* edge_brush = nullptr;
            Rect edge_bounds{};
            auto bounds = transformed_box(image_transform, {0, 0, 1, 1});
            if (aligned && decoded->stencil) {
                // A scanned page can paint a black base, then cover its white
                // pixels with a stencil. Cover the complete boundary pixels;
                // partial edge coverage would expose a gray frame underneath.
                bounds = {std::floor(bounds.x0), std::floor(bounds.y0),
                          std::ceil(bounds.x1), std::ceil(bounds.y1)};
                edge_bounds = transformed_box(inverse(image_transform), bounds);
                auto found = canvas.stencils.find(decoded.get());
                auto* entry = found != canvas.stencils.end() && found->second.bitmap == bitmap
                                  ? &found->second : nullptr;
                if (entry && entry->edge_brush) {
                    edge_brush = entry->edge_brush;
                    edge_brush->AddRef();
                } else {
                    auto properties = D2D1::BitmapBrushProperties(
                        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
                    auto result = target.canvas->target->CreateBitmapBrush(
                        bitmap, properties, D2D1::BrushProperties(1,
                            D2D1::Matrix3x2F::Scale(1.f / decoded->width, 1.f / decoded->height)), &edge_brush);
                    if (FAILED(result)) {
                        release(bitmap);
                        hr(result, "Create stencil edge brush");
                    }
                    if (entry) {
                        entry->edge_brush = edge_brush;
                        edge_brush->AddRef();
                    }
                }
                edge_brush->SetOpacity(target.g.fill_alpha);
                edge_brush->SetInterpolationMode(interpolate ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                                            : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            bool change_clip_edges = aligned && rect_clip_affects_edges(target, bounds);
            if (change_clip_edges)
                set_rect_clip_antialias(target, D2D1_ANTIALIAS_MODE_ALIASED);
            if (aligned)
                target.canvas->target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            if (edge_brush)
                target.canvas->target->FillRectangle(D2D1::RectF(float(edge_bounds.x0), float(edge_bounds.y0),
                    float(edge_bounds.x1), float(edge_bounds.y1)), edge_brush);
            else
                target.canvas->target->DrawBitmap(bitmap, D2D1::RectF(0, 0, 1, 1), target.g.fill_alpha,
                    interpolate ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            target.canvas->target->SetAntialiasMode(antialias);
            // Vector strokes can lie on a fractional clip boundary. Restore
            // edge coverage so those strokes are not discarded with a pixel.
            if (change_clip_edges)
                set_rect_clip_antialias(target, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            release(edge_brush);
            release(bitmap);
        },
        false, 1, transformed_box(c.g.ctm, {0, 0, 1, 1}), false, image_mask);
}
static Rect transformed_box(Matrix m, Rect r) {
    Point p[] = {transform(m, {r.x0, r.y0}), transform(m, {r.x1, r.y0}), transform(m, {r.x1, r.y1}),
                 transform(m, {r.x0, r.y1})};
    Rect out{p[0].x, p[0].y, p[0].x, p[0].y};
    for (auto q : p) {
        out.x0 = std::min(out.x0, q.x);
        out.x1 = std::max(out.x1, q.x);
        out.y0 = std::min(out.y0, q.y);
        out.y1 = std::max(out.y1, q.y);
    }
    return out;
}
static std::shared_ptr<Bytes> type3_content(Renderer& r, Document& d, const Value& procedure) {
    if (auto found = r.type3_contents.find(&procedure); found != r.type3_contents.end())
        return found->second;
    auto bytes = std::make_shared<Bytes>(decode_stream(d, procedure).bytes);
    size_t cost = bytes->size() + 256;
    constexpr size_t limit = 16 * 1024 * 1024;
    if (cost <= limit) {
        if (r.type3_content_bytes + cost > limit || r.type3_contents.size() >= 4096) {
            r.type3_contents.clear();
            r.type3_content_bytes = 0;
        }
        r.type3_contents.emplace(&procedure, bytes);
        r.type3_content_bytes += cost;
    }
    return bytes;
}
static void show_text(Context& c, std::string_view text, const Value& resources,
                      std::vector<ID2D1Geometry*>& text_clips) {
    if (!c.g.font) {
        warn(*c.document, "Text without a selected font was skipped");
        return;
    }
    auto& f = *c.g.font;
    auto glyphs = decode_text(f, text);
    if (glyphs.empty())
        return;
    double fs = c.g.font_size;
    if (std::abs(fs) < 1e-9)
        return;
    std::vector<uint16_t> indices;
    std::vector<float> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    std::wstring extracted;
    double advance = 0;
    Matrix text_matrix = multiply(c.g.ctm, multiply(c.g.text, {c.g.hscale * fs, 0, 0, -fs, 0, c.g.rise}));
    for (auto& glyph : glyphs) {
        indices.push_back(glyph.index);
        double a = (f.cmap.vertical ? glyph.vadvance : glyph.advance) * fs / 1000 + c.g.char_space +
                   (glyph.code == 32 ? c.g.word_space : 0);
        advances.push_back(float(a / fs));
        offsets.push_back({0, 0});
        extracted += glyph.text;
        advance += a;
    }
    if (c.collect_text && !extracted.empty()) {
        double width = f.cmap.vertical ? fs : advance * c.g.hscale;
        Rect box{std::min(0.0, width), f.descent * fs / 1000 + c.g.rise, std::max(0.0, width),
                 f.ascent * fs / 1000 + c.g.rise};
        if (f.cmap.vertical)
            box = {-fs / 2, advance, fs / 2, fs};
        c.output->text.push_back({extracted, transformed_box(multiply(c.g.ctm, c.g.text), box)});
    }
    bool fill = c.g.text_mode == 0 || c.g.text_mode == 2 || c.g.text_mode == 4 || c.g.text_mode == 6;
    bool stroke = c.g.text_mode == 1 || c.g.text_mode == 2 || c.g.text_mode == 5 || c.g.text_mode == 6;
    bool clip = c.g.text_mode >= 4;
    auto glyph_shape = [&](const std::vector<Segment>& path, Matrix mat) {
        std::vector<Segment> transformed = path;
        for (auto& seg : transformed) {
            seg.p1 = transform(mat, seg.p1);
            seg.p2 = transform(mat, seg.p2);
            seg.p3 = transform(mat, seg.p3);
        }
        if (fill || stroke)
            draw_path(c, resources, transformed, fill, stroke, false);
        if (clip)
            text_clips.push_back(geometry(c, transformed));
    };
    if (c.text_only) {
    } else if (f.type3 || f.type1) {
        double position = 0;
        for (size_t i = 0; i < glyphs.size(); ++i) {
            auto& g = glyphs[i];
            Matrix local =
                multiply(c.g.ctm, multiply(c.g.text,
                                           {fs * c.g.hscale, 0, 0, fs,
                                            f.cmap.vertical ? -g.vx * fs / 1000 : position * c.g.hscale,
                                            c.g.rise + (f.cmap.vertical ? position - g.vy * fs / 1000 : 0)}));
            if (f.type1)
                glyph_shape(type1_outline(f, g.name), multiply(local, {0.001, 0, 0, 0.001, 0, 0}));
            else if (fill || stroke) {
                auto& procedure = entry(*c.document, entry(*c.document, f.dictionary, "CharProcs"), g.name);
                if (procedure.has_stream) {
                    auto saved = c.g;
                    bool saved_glyph_image = c.glyph_image;
                    size_t clips = c.clips.size();
                    c.g.ctm = multiply(local, f.matrix);
                    auto& font_resources = entry(*c.document, f.dictionary, "Resources");
                    auto bytes = type3_content(*c.renderer, *c.document, procedure);
                    c.glyph_image = true;
                    try {
                        interpret(c, *bytes, font_resources.kind == Kind::null ? resources : font_resources,
                                  c.depth + 1);
                    } catch (...) {
                        c.glyph_image = saved_glyph_image;
                        pop_clips(c, clips);
                        c.g = saved;
                        throw;
                    }
                    c.glyph_image = saved_glyph_image;
                    pop_clips(c, clips);
                    c.g = saved;
                }
            }
            position += advances[i] * fs;
        }
    } else if (f.face) {
        auto draw_run = [&](size_t start, size_t count, Matrix mat, const float* advances_data) {
            // DirectWrite's raw CFF rasterizer can make thin stems too heavy.
            // Its unhinted outlines retain the embedded font's stroke widths.
            bool outline_fill = fill && f.face->GetType() == DWRITE_FONT_FACE_TYPE_RAW_CFF;
            DWRITE_GLYPH_RUN run{f.face,        1.0f,    UINT32(count), indices.data() + start,
                                 advances_data, nullptr, FALSE,         0};
            ID2D1PathGeometry* outline = nullptr;
            ID2D1TransformedGeometry* transformed = nullptr;
            ID2D1GeometrySink* sink = nullptr;
            try {
                if (stroke || clip || outline_fill) {
                    hr(c.renderer->d2d->CreatePathGeometry(&outline), "Create glyph outlines");
                    hr(outline->Open(&sink), "Open glyph outline sink");
                    hr(f.face->GetGlyphRunOutline(1, run.glyphIndices, run.glyphAdvances, nullptr,
                                                  run.glyphCount, FALSE, FALSE, sink),
                       "Read font glyph outlines");
                    hr(sink->Close(), "Close glyph outlines");
                    release(sink);
                    if (stroke || clip)
                        hr(c.renderer->d2d->CreateTransformedGeometry(outline, matrix(mat), &transformed),
                           "Transform glyph outlines");
                }
                if (fill || stroke)
                    paint(c, resources, [&](Context& target) {
                        auto rt = target.canvas->target;
                        ID2D1Brush* brush = nullptr;
                        try {
                            if (fill) {
                                brush = make_brush(target, resources, false);
                                auto inv = inverse(mat);
                                D2D1_MATRIX_3X2_F bm;
                                brush->GetTransform(&bm);
                                brush->SetTransform(bm * matrix(inv));
                                set_transform(target, mat);
                                if (outline_fill)
                                    rt->FillGeometry(outline, brush);
                                else
                                    rt->DrawGlyphRun({0, 0}, &run, brush, DWRITE_MEASURING_MODE_NATURAL);
                                release(brush);
                            }
                            if (stroke) {
                                brush = make_brush(target, resources, true);
                                set_transform(target);
                                double scale = std::sqrt(std::abs(target.g.ctm.a * target.g.ctm.d -
                                                                  target.g.ctm.b * target.g.ctm.c));
                                rt->DrawGeometry(transformed, brush,
                                                 float(std::max(0.5, target.g.width * scale)));
                                release(brush);
                            }
                        } catch (...) {
                            release(brush);
                            throw;
                        }
                    });
                if (clip && transformed) {
                    transformed->AddRef();
                    text_clips.push_back(transformed);
                }
                release(transformed);
                release(outline);
            } catch (...) {
                release(sink);
                release(transformed);
                release(outline);
                throw;
            }
        };
        if (!f.cmap.vertical)
            draw_run(0, indices.size(), text_matrix, advances.data());
        else {
            double position = 0;
            for (size_t i = 0; i < glyphs.size(); ++i) {
                Matrix mat =
                    multiply(c.g.ctm, multiply(c.g.text, {fs * c.g.hscale, 0, 0, -fs,
                                                          -glyphs[i].vx * fs * c.g.hscale / 1000,
                                                          position - glyphs[i].vy * fs / 1000 + c.g.rise}));
                draw_run(i, 1, mat, advances.data() + i);
                position += advances[i] * fs;
            }
        }
    }
    c.g.text = multiply(c.g.text, f.cmap.vertical ? Matrix{1, 0, 0, 1, 0, advance}
                                                  : Matrix{1, 0, 0, 1, advance * c.g.hscale, 0});
}
static void finish_text_clip(Context& c, std::vector<ID2D1Geometry*>& clips) {
    if (clips.empty())
        return;
    ID2D1GeometryGroup* group = nullptr;
    hr(c.renderer->d2d->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, clips.data(), UINT32(clips.size()),
                                            &group),
       "Combine text clipping shapes");
    push_clip(c, group);
    release(group);
    for (auto shape : clips)
        release(shape);
    clips.clear();
}

static std::shared_ptr<Image> shading_image(Context& c, const Value& input, const Value& resources,
                                           Rect* region = nullptr) {
    ProfileTimer timer(c.renderer->profiling ? &c.renderer->profile.shading_ms : nullptr);
    auto& d = *c.document;
    auto& sh = resolve(d, input);
    int type = integer(get(sh, "ShadingType"));
    auto space = color_space(d, get(sh, "ColorSpace"), resources);
    auto& function = get(sh, "Function");
    auto& coords = entry(d, sh, "Coords");
    auto& domain = entry(d, sh, "Domain");
    auto& extend = entry(d, sh, "Extend");
    bool ex0 = boolean(at(extend, 0)), ex1 = boolean(at(extend, 1));
    auto image = std::make_shared<Image>();
    image->width = c.canvas->width;
    image->height = c.canvas->height;
    int origin_x = c.origin_x, origin_y = c.origin_y;
    if (region) {
        Rect bounds{double(origin_x), double(origin_y), double(origin_x) + image->width,
                    double(origin_y) + image->height};
        auto intersect = [&](Rect box) {
            bounds.x0 = std::max(bounds.x0, box.x0 - 1);
            bounds.y0 = std::max(bounds.y0, box.y0 - 1);
            bounds.x1 = std::min(bounds.x1, box.x1 + 1);
            bounds.y1 = std::min(bounds.y1, box.y1 + 1);
        };
        for (auto clip : c.clips) {
            auto box = clip.bounds;
            intersect({box.left, box.top, box.right, box.bottom});
        }
        auto& box = entry(d, sh, "BBox");
        if (box.kind == Kind::array)
            intersect(transformed_box(c.g.ctm, rectangle(box)));
        if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) {
            *region = {0, 0, 0, 0};
            return image;
        }
        origin_x = int(std::floor(bounds.x0));
        origin_y = int(std::floor(bounds.y0));
        image->width = unsigned(std::ceil(bounds.x1) - origin_x);
        image->height = unsigned(std::ceil(bounds.y1) - origin_y);
        *region = {double(origin_x), double(origin_y), double(origin_x) + image->width,
                   double(origin_y) + image->height};
    }
    image->pixels.assign(size_t(image->width) * image->height * 4, 0);
    Matrix inv = inverse(c.g.ctm);
    auto mesh_point = [&](double x, double y) {
        auto p = transform(c.g.ctm, {x, y});
        p.x -= origin_x;
        p.y -= origin_y;
        return p;
    };
    Rect bbox = rectangle(entry(d, sh, "BBox"), {-1e30, -1e30, 1e30, 1e30});
    auto& bg = entry(d, sh, "Background");
    Color background{0, 0, 0, 0};
    if (bg.kind == Kind::array) {
        std::vector<double> v;
        for (auto& x : bg.array)
            v.push_back(num(x));
        background = color(d, space, v);
    }
    auto set = [&](unsigned x, unsigned y, Color value) {
        size_t p = (size_t(y) * image->width + x) * 4;
        image->pixels[p] = uint8_t(std::clamp(value.b * value.a, 0.f, 1.f) * 255 + 0.5f);
        image->pixels[p + 1] = uint8_t(std::clamp(value.g * value.a, 0.f, 1.f) * 255 + 0.5f);
        image->pixels[p + 2] = uint8_t(std::clamp(value.r * value.a, 0.f, 1.f) * 255 + 0.5f);
        image->pixels[p + 3] = uint8_t(std::clamp(value.a, 0.f, 1.f) * 255 + 0.5f);
    };
    if (type == 2 || type == 3) {
        std::array<Color, 1025> stops{};
        double d0 = num(at(domain, 0)), d1 = num(at(domain, 1), 1);
        const auto* direct = &space;
        while (direct->name == "ICCBased" && direct->alternate)
            direct = direct->alternate.get();
        int kind = direct->name == "DeviceRGB" ? 3 : direct->name == "DeviceGray" ? 1 :
                   direct->name == "DeviceCMYK" ? 4 : 0;
        const Value* tint = nullptr;
        if (!kind && (direct->name == "Separation" || direct->name == "DeviceN") &&
            direct->alternate && direct->function.kind == Kind::ref) {
            auto base = direct->alternate.get();
            while (base->name == "ICCBased" && base->alternate)
                base = base->alternate.get();
            int base_kind = base->name == "DeviceRGB" ? 3 : base->name == "DeviceGray" ? 1 :
                            base->name == "DeviceCMYK" ? 4 : 0;
            if (base_kind) {
                kind = 16 + 256 * direct->components + base_kind;
                tint = &resolve(d, direct->function);
            }
        }
        auto key = &resolve(d, function);
        auto definition = &entry(d, sh, "ColorSpace");
        auto& ramps = c.renderer->gradient_ramps;
        auto cached = ramps.find(key);
        if (cached != ramps.end() && cached->second.space == kind &&
            cached->second.low == d0 && cached->second.high == d1 && cached->second.tint == tint &&
            (kind || (cached->second.definition == definition && cached->second.resources == &resources)))
            stops = cached->second.colors;
        else {
            for (size_t i = 0; i < stops.size(); ++i) {
                double v = d0 + (d1 - d0) * i / (stops.size() - 1);
                auto components = evaluate_function(d, function, std::span(&v, 1));
                stops[i] = color(d, space, components);
            }
            if (ramps.size() >= 256)
                ramps.clear();
            ramps[key] = {d0, d1, kind, stops, definition, &resources, tint};
        }
        double x0 = num(at(coords, 0)), y0 = num(at(coords, 1)), r0 = type == 3 ? num(at(coords, 2)) : 0,
               x1 = num(at(coords, type == 3 ? 3 : 2)), y1 = num(at(coords, type == 3 ? 4 : 3)),
               r1 = type == 3 ? num(at(coords, 5)) : 0, dx = x1 - x0, dy = y1 - y0, dr = r1 - r0;
        for (unsigned y = 0; y < image->height; ++y) {
            if ((y & 63) == 0)
                check_cancel(d);
            for (unsigned x = 0; x < image->width; ++x) {
                Point p = transform(inv, {x + 0.5 + origin_x, y + 0.5 + origin_y});
                if (p.x < bbox.x0 || p.x > bbox.x1 || p.y < bbox.y0 || p.y > bbox.y1)
                    continue;
                double px = p.x - x0, py = p.y - y0, t = 0;
                bool found = true;
                if (type == 2) {
                    double den = dx * dx + dy * dy;
                    if (den < 1e-20)
                        found = false;
                    else
                        t = (px * dx + py * dy) / den;
                } else {
                    double a = dx * dx + dy * dy - dr * dr, b = -2 * (px * dx + py * dy + r0 * dr),
                           cc = px * px + py * py - r0 * r0;
                    double root1 = 0, root2 = 0;
                    if (std::abs(a) < 1e-16) {
                        if (std::abs(b) < 1e-16)
                            found = false;
                        else
                            root1 = root2 = -cc / b;
                    } else {
                        double disc = b * b - 4 * a * cc;
                        if (disc < 0)
                            found = false;
                        else {
                            root1 = (-b - std::sqrt(disc)) / (2 * a);
                            root2 = (-b + std::sqrt(disc)) / (2 * a);
                        }
                    }
                    if (found) {
                        if (root1 > root2)
                            std::swap(root1, root2);
                        auto valid = [&](double root) {
                            return r0 + root * dr >= 0 && (root >= 0 || ex0) && (root <= 1 || ex1);
                        };
                        if (valid(root2))
                            t = root2;
                        else if (valid(root1))
                            t = root1;
                        else
                            found = false;
                    }
                }
                if (!found || (t < 0 && !ex0) || (t > 1 && !ex1)) {
                    set(x, y, background);
                    continue;
                }
                double z = std::clamp(t, 0.0, 1.0) * 1024;
                unsigned i = unsigned(z);
                unsigned j = std::min(i + 1, 1024u);
                float f = float(z - i);
                auto a = stops[i], b = stops[j];
                set(x, y, {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1});
            }
        }
    } else if (type == 1) {
        auto fn_matrix = value_matrix(entry(d, sh, "Matrix"));
        auto to_function = inverse(multiply(c.g.ctm, fn_matrix));
        double x0 = num(at(domain, 0)), x1 = num(at(domain, 1), 1), y0 = num(at(domain, 2)),
               y1 = num(at(domain, 3), 1);
        require(x1 > x0 && y1 > y0, "Invalid function shading domain");
        constexpr unsigned grid = 128;
        std::vector<Color> samples((grid + 1) * (grid + 1));
        for (unsigned y = 0; y <= grid; ++y)
            for (unsigned x = 0; x <= grid; ++x) {
                double values[] = {x0 + (x1 - x0) * x / grid, y0 + (y1 - y0) * y / grid};
                samples[y * (grid + 1) + x] = color(d, space, evaluate_function(d, function, values));
            }
        for (unsigned y = 0; y < image->height; ++y)
            for (unsigned x = 0; x < image->width; ++x) {
                auto p = transform(to_function, {x + 0.5 + origin_x, y + 0.5 + origin_y});
                if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1)
                    continue;
                double fx = (p.x - x0) / (x1 - x0) * grid, fy = (p.y - y0) / (y1 - y0) * grid;
                unsigned ix = std::min(unsigned(fx), grid - 1), iy = std::min(unsigned(fy), grid - 1);
                fx -= ix;
                fy -= iy;
                Color value{0, 0, 0, 1};
                for (unsigned j = 0; j < 2; ++j)
                    for (unsigned i = 0; i < 2; ++i) {
                        float w = float((i ? fx : 1 - fx) * (j ? fy : 1 - fy));
                        auto v = samples[(iy + j) * (grid + 1) + ix + i];
                        value.r += v.r * w;
                        value.g += v.g * w;
                        value.b += v.b * w;
                    }
                set(x, y, value);
            }
    } else if (type == 4 || type == 5) {
        auto data = decode_stream(d, sh).bytes;
        unsigned bits_coord = unsigned(integer(get(sh, "BitsPerCoordinate"))),
                 bits_component = unsigned(integer(get(sh, "BitsPerComponent"))),
                 bits_flag = unsigned(integer(get(sh, "BitsPerFlag")));
        require(bits_coord >= 1 && bits_coord <= 32 && bits_component >= 1 && bits_component <= 16 &&
                    (type == 5 || (bits_flag >= 1 && bits_flag <= 8)),
                "Invalid mesh bit widths");
        auto& dec = entry(d, sh, "Decode");
        unsigned components = function.kind == Kind::null ? unsigned(space.components) : 1;
        size_t bit = 0;
        auto read = [&](unsigned count) {
            require(bit + count <= data.size() * 8, "Truncated mesh data");
            uint32_t value = 0;
            for (unsigned i = 0; i < count; ++i) {
                value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
                ++bit;
            }
            return value;
        };
        auto component = [&](unsigned count, unsigned index) {
            double lo = num(at(dec, index * 2)), hi = num(at(dec, index * 2 + 1), 1);
            return lo + double(read(count)) * (hi - lo) / double((uint64_t(1) << count) - 1);
        };
        struct Vertex {
            Point p;
            Color c;
        };
        std::vector<Vertex> vertices;
        auto vertex = [&] {
            double x = component(bits_coord, 0), y = component(bits_coord, 1);
            std::vector<double> values;
            for (unsigned i = 0; i < components; ++i)
                values.push_back(component(bits_component, 2 + i));
            if (function.kind != Kind::null)
                values = evaluate_function(d, function, values);
            return Vertex{mesh_point(x, y), color(d, space, values)};
        };
        auto triangle = [&](Vertex a, Vertex b, Vertex cc) {
            double area = (b.p.x - a.p.x) * (cc.p.y - a.p.y) - (b.p.y - a.p.y) * (cc.p.x - a.p.x);
            if (std::abs(area) < 1e-12)
                return;
            int x0 = std::max(0, int(std::floor(std::min({a.p.x, b.p.x, cc.p.x})))),
                y0 = std::max(0, int(std::floor(std::min({a.p.y, b.p.y, cc.p.y})))),
                x1 = std::min(int(image->width), int(std::ceil(std::max({a.p.x, b.p.x, cc.p.x})))),
                y1 = std::min(int(image->height), int(std::ceil(std::max({a.p.y, b.p.y, cc.p.y}))));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    double px = x + 0.5, py = y + 0.5;
                    double u = ((b.p.x - px) * (cc.p.y - py) - (b.p.y - py) * (cc.p.x - px)) / area,
                           v = ((cc.p.x - px) * (a.p.y - py) - (cc.p.y - py) * (a.p.x - px)) / area,
                           w = 1 - u - v;
                    if (u >= -1e-6 && v >= -1e-6 && w >= -1e-6)
                        set(unsigned(x), unsigned(y),
                            {float(a.c.r * u + b.c.r * v + cc.c.r * w),
                             float(a.c.g * u + b.c.g * v + cc.c.g * w),
                             float(a.c.b * u + b.c.b * v + cc.c.b * w), 1});
                }
        };
        if (type == 5) {
            unsigned row = unsigned(integer(get(sh, "VerticesPerRow")));
            require(row >= 2 && row <= 100000, "Invalid lattice mesh row size");
            size_t record = 2 * bits_coord + components * bits_component;
            while (data.size() * 8 - bit >= record) {
                vertices.push_back(vertex());
                bit = (bit + 7) & ~size_t(7);
                require(vertices.size() < 1000000, "Mesh vertex limit");
            }
            for (size_t i = row; i + 1 < vertices.size(); ++i)
                if (i % row + 1 < row) {
                    triangle(vertices[i - row], vertices[i], vertices[i - row + 1]);
                    triangle(vertices[i], vertices[i - row + 1], vertices[i + 1]);
                }
        } else {
            Vertex previous[3]{};
            bool have = false;
            size_t record = bits_flag + 2 * bits_coord + components * bits_component;
            while (data.size() * 8 - bit >= record) {
                unsigned flag = read(bits_flag);
                Vertex v = vertex();
                bit = (bit + 7) & ~size_t(7);
                if (flag == 0) {
                    previous[0] = v;
                    require(data.size() * 8 - bit >= 2 * record, "Incomplete mesh triangle");
                    read(bits_flag);
                    previous[1] = vertex();
                    bit = (bit + 7) & ~size_t(7);
                    read(bits_flag);
                    previous[2] = vertex();
                    bit = (bit + 7) & ~size_t(7);
                    have = true;
                } else {
                    require(have && (flag == 1 || flag == 2), "Invalid mesh triangle flag");
                    if (flag == 1)
                        previous[0] = previous[1];
                    previous[1] = previous[2];
                    previous[2] = v;
                }
                triangle(previous[0], previous[1], previous[2]);
            }
        }
    } else if (type == 6 || type == 7) {
        auto data = decode_stream(d, sh).bytes;
        unsigned coordinate_bits = unsigned(integer(get(sh, "BitsPerCoordinate"))),
                 component_bits = unsigned(integer(get(sh, "BitsPerComponent"))),
                 flag_bits = unsigned(integer(get(sh, "BitsPerFlag")));
        require(coordinate_bits >= 1 && coordinate_bits <= 32 && component_bits >= 1 &&
                    component_bits <= 16 && (flag_bits == 2 || flag_bits == 4 || flag_bits == 8),
                "Invalid patch mesh bit widths");
        unsigned components = function.kind == Kind::null ? unsigned(space.components) : 1;
        auto& dec = entry(d, sh, "Decode");
        require(dec.array.size() == 4 + components * 2, "Invalid patch mesh Decode array");
        size_t bit = 0, patches = 0, triangles = 0;
        auto read = [&](unsigned count) {
            require(bit + count <= data.size() * 8, "Truncated patch mesh");
            uint32_t value = 0;
            for (unsigned i = 0; i < count; ++i) {
                value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
                ++bit;
            }
            return value;
        };
        auto sample = [&](unsigned bits, unsigned index) {
            double lo = num(at(dec, index * 2)), hi = num(at(dec, index * 2 + 1), 1);
            return lo + double(read(bits)) * (hi - lo) / double((uint64_t(1) << bits) - 1);
        };
        std::array<Point, 16> previous_points{};
        std::array<std::vector<double>, 4> previous_colors;
        struct Vertex {
            Point p;
            Color c;
        };
        auto triangle = [&](Vertex a, Vertex b, Vertex cc) {
            require(++triangles < 8'000'000, "Patch tessellation budget exceeded");
            double area = (b.p.x - a.p.x) * (cc.p.y - a.p.y) - (b.p.y - a.p.y) * (cc.p.x - a.p.x);
            if (std::abs(area) < 1e-12)
                return;
            double left = std::clamp(std::floor(std::min({a.p.x, b.p.x, cc.p.x})), 0.0, double(image->width)),
                   right = std::clamp(std::ceil(std::max({a.p.x, b.p.x, cc.p.x})), 0.0, double(image->width));
            double top = std::clamp(std::floor(std::min({a.p.y, b.p.y, cc.p.y})), 0.0, double(image->height)),
                   bottom =
                       std::clamp(std::ceil(std::max({a.p.y, b.p.y, cc.p.y})), 0.0, double(image->height));
            for (unsigned y = unsigned(top); y < unsigned(bottom); ++y)
                for (unsigned x = unsigned(left); x < unsigned(right); ++x) {
                    double px = x + 0.5, py = y + 0.5;
                    double u = ((b.p.x - px) * (cc.p.y - py) - (b.p.y - py) * (cc.p.x - px)) / area,
                           v = ((cc.p.x - px) * (a.p.y - py) - (cc.p.y - py) * (a.p.x - px)) / area,
                           w = 1 - u - v;
                    if (u >= -1e-6 && v >= -1e-6 && w >= -1e-6)
                        set(x, y,
                            {float(a.c.r * u + b.c.r * v + cc.c.r * w),
                             float(a.c.g * u + b.c.g * v + cc.c.g * w),
                             float(a.c.b * u + b.c.b * v + cc.c.b * w), 1});
                }
        };
        auto bernstein = [](double t) {
            double q = 1 - t;
            return std::array<double, 4>{q * q * q, 3 * t * q * q, 3 * t * t * q, t * t * t};
        };
        while (data.size() * 8 - bit >= flag_bits) {
            check_cancel(d);
            unsigned flag = read(flag_bits) & 3;
            require(flag == 0 || patches > 0, "First mesh patch must define all points");
            require(++patches <= 100000, "Patch mesh count limit exceeded");
            std::array<Point, 16> points{};
            std::array<std::vector<double>, 4> colors;
            if (flag) {
                for (unsigned i = 0; i < 4; ++i)
                    points[i] = previous_points[(flag * 3 + i) % 12];
                colors[0] = previous_colors[flag];
                colors[1] = previous_colors[(flag + 1) % 4];
            }
            unsigned count = type == 6 ? 12 : 16;
            for (unsigned i = flag ? 4 : 0; i < count; ++i) {
                double x = sample(coordinate_bits, 0), y = sample(coordinate_bits, 1);
                points[i] = mesh_point(x, y);
            }
            for (unsigned i = flag ? 2 : 0; i < 4; ++i)
                for (unsigned j = 0; j < components; ++j)
                    colors[i].push_back(sample(component_bits, j + 2));
            bit = (bit + 7) & ~size_t(7);
            double left = points[0].x, right = left, top = points[0].y, bottom = top;
            for (unsigned i = 0; i < count; ++i) {
                left = std::min(left, points[i].x);
                right = std::max(right, points[i].x);
                top = std::min(top, points[i].y);
                bottom = std::max(bottom, points[i].y);
            }
            unsigned divisions =
                unsigned(std::clamp(std::ceil(std::sqrt(std::max(right - left, bottom - top))), 8.0, 96.0));
            std::vector<Vertex> grid(size_t(divisions + 1) * (divisions + 1));
            auto curve = [&](unsigned a, unsigned b, unsigned cc, unsigned e,
                             const std::array<double, 4>& basis) {
                return Point{points[a].x * basis[0] + points[b].x * basis[1] + points[cc].x * basis[2] +
                                 points[e].x * basis[3],
                             points[a].y * basis[0] + points[b].y * basis[1] + points[cc].y * basis[2] +
                                 points[e].y * basis[3]};
            };
            for (unsigned y = 0; y <= divisions; ++y)
                for (unsigned x = 0; x <= divisions; ++x) {
                    double u = double(x) / divisions, v = double(y) / divisions;
                    auto bu = bernstein(u), bv = bernstein(v);
                    double weights[] = {(1 - u) * (1 - v), (1 - u) * v, u * v, u * (1 - v)};
                    Point p{};
                    if (type == 6) {
                        auto bot = curve(0, 11, 10, 9, bu), top_curve = curve(3, 4, 5, 6, bu),
                             left_curve = curve(0, 1, 2, 3, bv), right_curve = curve(9, 8, 7, 6, bv);
                        p = {(1 - v) * bot.x + v * top_curve.x + (1 - u) * left_curve.x + u * right_curve.x,
                             (1 - v) * bot.y + v * top_curve.y + (1 - u) * left_curve.y + u * right_curve.y};
                        for (unsigned i = 0; i < 4; ++i) {
                            p.x -= weights[i] * points[i * 3].x;
                            p.y -= weights[i] * points[i * 3].y;
                        }
                    } else {
                        constexpr unsigned order[] = {0, 11, 10, 9, 1, 12, 15, 8, 2, 13, 14, 7, 3, 4, 5, 6};
                        for (unsigned j = 0; j < 4; ++j)
                            for (unsigned i = 0; i < 4; ++i) {
                                auto q = points[order[j * 4 + i]];
                                double weight = bu[i] * bv[j];
                                p.x += q.x * weight;
                                p.y += q.y * weight;
                            }
                    }
                    std::vector<double> values(components, 0);
                    for (unsigned i = 0; i < 4; ++i)
                        for (unsigned j = 0; j < components; ++j)
                            values[j] += weights[i] * colors[i][j];
                    if (function.kind != Kind::null)
                        values = evaluate_function(d, function, values);
                    grid[size_t(y) * (divisions + 1) + x] = {p, color(d, space, values)};
                }
            for (unsigned y = 0; y < divisions; ++y)
                for (unsigned x = 0; x < divisions; ++x) {
                    size_t index = size_t(y) * (divisions + 1) + x;
                    triangle(grid[index], grid[index + 1], grid[index + divisions + 1]);
                    triangle(grid[index + 1], grid[index + divisions + 2], grid[index + divisions + 1]);
                }
            previous_points = points;
            previous_colors = std::move(colors);
        }
    } else {
        warn(d, "Unsupported shading type " + std::to_string(type));
    }
    return image;
}
static ID2D1Brush* bitmap_brush(Context& c, const Image& image, Matrix transform_value, float opacity,
                                bool tile) {
    ID2D1Bitmap* bitmap = nullptr;
    ID2D1BitmapBrush* brush = nullptr;
    hr(c.canvas->target->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(),
                                      image.width * 4,
                                      D2D1::BitmapProperties(D2D1::PixelFormat(
                                          DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                      &bitmap),
       "Create paint bitmap");
    auto properties = D2D1::BitmapBrushProperties(tile ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                  tile ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    auto result = c.canvas->target->CreateBitmapBrush(
        bitmap, properties, D2D1::BrushProperties(opacity, matrix(transform_value)), &brush);
    release(bitmap);
    hr(result, "Create bitmap paint");
    return brush;
}
static ID2D1Brush* make_brush(Context& c, const Value& resources, bool stroke) {
    auto& pattern_name = stroke ? c.g.stroke_pattern : c.g.fill_pattern;
    float alpha = stroke ? c.g.stroke_alpha : c.g.fill_alpha;
    if (pattern_name.empty()) {
        auto& brush = c.canvas->solid_brush;
        auto value = rgba(stroke ? c.g.stroke : c.g.fill, alpha);
        if (!brush)
            hr(c.canvas->target->CreateSolidColorBrush(value, &brush), "Create color paint");
        else
            brush->SetColor(value);
        brush->SetTransform(D2D1::Matrix3x2F::Identity());
        brush->AddRef();
        return brush;
    }
    require(c.depth < 24, "Pattern recursion limit");
    auto& pattern = entry(*c.document, entry(*c.document, resources, "Pattern"), pattern_name);
    int type = integer(get(pattern, "PatternType"));
    Matrix pattern_matrix = multiply(c.g.ctm, value_matrix(entry(*c.document, pattern, "Matrix")));
    if (type == 2) {
        Context sub = c;
        sub.g.ctm = pattern_matrix;
        auto image = shading_image(sub, get(pattern, "Shading"), resources);
        return bitmap_brush(c, *image, {1, 0, 0, 1, double(c.origin_x), double(c.origin_y)}, alpha, false);
    }
    require(type == 1 && pattern.has_stream, "Invalid tiling pattern");
    double xs = std::abs(num(entry(*c.document, pattern, "XStep"))),
           ys = std::abs(num(entry(*c.document, pattern, "YStep")));
    require(xs > 1e-8 && ys > 1e-8, "Invalid tiling pattern step");
    Rect box = rectangle(entry(*c.document, pattern, "BBox"));
    double sx = std::hypot(pattern_matrix.a, pattern_matrix.b),
           sy = std::hypot(pattern_matrix.c, pattern_matrix.d);
    unsigned w = unsigned(std::clamp(std::ceil(xs * sx), 1.0, 2048.0)),
             h = unsigned(std::clamp(std::ceil(ys * sy), 1.0, 2048.0));
    auto tile = surface(*c.renderer, w, h);
    Context sub;
    sub.renderer = c.renderer;
    sub.document = c.document;
    sub.canvas = tile.get();
    sub.output = c.output;
    sub.collect_text = false;
    sub.g = c.g;
    sub.alpha_only = c.alpha_only;
    sub.g.fill_pattern.clear();
    sub.g.stroke_pattern.clear();
    sub.g.blend = "Normal";
    sub.g.softmask = {};
    sub.g.fill_alpha = sub.g.stroke_alpha = 1;
    sub.depth = c.depth + 1;
    auto bytes = decode_stream(*c.document, pattern).bytes;
    auto& pattern_resources = entry(*c.document, pattern, "Resources");
    tile->target->BeginDraw();
    tile->target->Clear(D2D1::ColorF(0, 0));
    try {
        int x0 = int(std::floor(-box.x1 / xs)) + 1, x1 = int(std::ceil((xs - box.x0) / xs)),
            y0 = int(std::floor(-box.y1 / ys)) + 1, y1 = int(std::ceil((ys - box.y0) / ys));
        require(int64_t(x1 - x0) * int64_t(y1 - y0) <= 256, "Tiling pattern overlap limit exceeded");
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                auto saved = sub.g;
                sub.g.ctm = {w / xs, 0, 0, h / ys, double(x) * w, double(y) * h};
                clip_rect(sub, box, sub.g.ctm);
                interpret(sub, bytes, pattern_resources.kind == Kind::null ? resources : pattern_resources,
                          sub.depth);
                pop_clips(sub, 0);
                sub.g = saved;
            }
        hr(tile->target->EndDraw(), "Render tiling pattern");
    } catch (...) {
        pop_clips(sub, 0);
        tile->target->EndDraw();
        throw;
    }
    Image image{w, h, pixels(*tile), false};
    return bitmap_brush(c, image, multiply(pattern_matrix, {xs / w, 0, 0, ys / h, 0, 0}), alpha, true);
}
static void draw_form(Context& c, const Value& input, const Value& resources, int depth, bool group) {
    require(depth < 48, "Form recursion limit exceeded");
    auto& d = *c.document;
    auto& form = resolve(d, input);
    if (!visible(d, get(form, "OC")))
        return;
    bool transparency = named(get(entry(d, form, "Group"), "S"), "Transparency");
    if (group && transparency && !c.text_only) {
        auto& attributes = entry(d, form, "Group");
        // With full group opacity and normal composition, a non-isolated group
        // can draw against the actual backdrop. This is both exact and avoids
        // allocating and scanning a page-sized surface for every form.
        if (!boolean(get(attributes, "I")) && !boolean(get(attributes, "K")) && c.g.fill_alpha == 1 &&
            (c.g.blend == "Normal" || c.g.blend == "Compatible") && c.g.softmask.kind == Kind::null) {
            auto saved = c.g;
            c.g.stroke_alpha = 1;
            try {
                draw_form(c, form, resources, depth, false);
                c.g = saved;
            } catch (...) {
                c.g = saved;
                throw;
            }
            return;
        }
    }
    if (group && transparency && !c.text_only) {
        auto& grp = entry(d, form, "Group");
        bool non_isolated = !boolean(get(grp, "I"));
        bool backdrop = non_isolated && !c.alpha_only;
        if (boolean(get(grp, "K")))
            warn(d, "Knockout transparency groups are approximated as non-knockout groups");
        float alpha = c.g.fill_alpha;
        paint(
            c, resources,
            [&](Context& sub) {
                sub.g.fill_alpha = sub.g.stroke_alpha = 1;
                draw_form(sub, form, resources, depth, false);
            },
            true, alpha,
            transformed_box(multiply(c.g.ctm, value_matrix(entry(d, form, "Matrix"))),
                            rectangle(entry(d, form, "BBox"))),
            backdrop);
        return;
    }
    auto saved = c.g;
    size_t count = c.clips.size();
    int old_depth = c.depth;
    c.depth = depth;
    try {
        c.g.ctm = multiply(c.g.ctm, value_matrix(entry(d, form, "Matrix")));
        if (get(form, "BBox").kind != Kind::null)
            clip_rect(c, rectangle(entry(d, form, "BBox")), c.g.ctm);
        auto& res = entry(d, form, "Resources");
        ProfileTimer decode_timer(c.renderer->profiling ? &c.renderer->profile.form_decode_ms : nullptr);
        auto bytes = decode_stream(d, form).bytes;
        decode_timer.stop();
        interpret(c, bytes, res.kind == Kind::null ? resources : res, depth);
        pop_clips(c, count);
        c.g = saved;
        c.depth = old_depth;
    } catch (...) {
        pop_clips(c, count);
        c.g = saved;
        c.depth = old_depth;
        throw;
    }
}
static void apply_ext(Context& c, const Value& value, const Value& resources) {
    auto& d = *c.document;
    auto& ext = resolve(d, value);
    auto& g = c.g;
    auto number = [&](std::string_view key, double& target) {
        auto& v = entry(d, ext, key);
        if (v.kind == Kind::number)
            target = v.number;
    };
    number("LW", g.width);
    number("ML", g.miter);
    if (get(ext, "LC").kind != Kind::null)
        g.cap = integer(get(ext, "LC"));
    if (get(ext, "LJ").kind != Kind::null)
        g.join = integer(get(ext, "LJ"));
    if (get(ext, "ca").kind != Kind::null)
        g.fill_alpha = float(std::clamp(num(get(ext, "ca")), 0.0, 1.0));
    if (get(ext, "CA").kind != Kind::null)
        g.stroke_alpha = float(std::clamp(num(get(ext, "CA")), 0.0, 1.0));
    auto& dash = entry(d, ext, "D");
    if (dash.kind == Kind::array) {
        g.dash.clear();
        for (auto& v : at(dash, 0).array)
            g.dash.push_back(float(std::max(0.0, num(v))));
        g.dash_offset = num(at(dash, 1));
    }
    auto& blend = entry(d, ext, "BM");
    if (blend.kind != Kind::null) {
        g.blend = blend.kind == Kind::array ? at(blend, 0).text : blend.text;
        static const std::unordered_set<std::string> modes = {
            "Normal",    "Compatible", "Multiply",   "Screen",    "Overlay",   "Darken",
            "Lighten",   "ColorDodge", "ColorBurn",  "HardLight", "SoftLight", "Difference",
            "Exclusion", "Hue",        "Saturation", "Color",     "Luminosity"};
        if (!modes.contains(g.blend)) {
            warn(d, "Unsupported blend mode " + g.blend);
            g.blend = "Normal";
        }
    }
    auto& smask = entry(d, ext, "SMask");
    if (smask.kind != Kind::null) {
        g.softmask = named(smask, "None") ? Value{} : get(ext, "SMask");
        g.mask_matrix = g.ctm;
    }
    auto& font = entry(d, ext, "Font");
    if (font.array.size() == 2) {
        g.font = &load_font(*c.renderer, d, at(font, 0));
        g.font_size = num(at(font, 1), 12);
    }
    if (boolean(get(ext, "OP")) || boolean(get(ext, "op")))
        warn(d, "Overprint is approximated in the RGB display output");
    if (get(ext, "TR").kind == Kind::ref || get(ext, "TR").kind == Kind::array)
        warn(d, "Graphics transfer functions are not applied");
    (void)resources;
}
static Value inline_image(Context& c, Parser& p, const Value& resources, std::shared_ptr<Image>& decoded) {
    size_t dictionary_start = p.pos;
    Value image;
    image.kind = Kind::dict;
    size_t entries = 0;
    static const std::unordered_map<std::string, std::string> aliases = {
        {"W", "Width"},       {"H", "Height"},     {"BPC", "BitsPerComponent"},
        {"CS", "ColorSpace"}, {"D", "Decode"},     {"DP", "DecodeParms"},
        {"F", "Filter"},      {"IM", "ImageMask"}, {"I", "Interpolate"}};
    for (;;) {
        auto key = parse(p);
        if (key.kind == Kind::keyword && key.text == "ID")
            break;
        require(key.kind == Kind::name && ++entries < 100, "Invalid inline image dictionary");
        auto v = parse(p);
        auto it = aliases.find(key.text);
        image.dict.emplace_back(it == aliases.end() ? key.text : it->second, std::move(v));
    }
    if (p.pos < p.data.size() && p.data[p.pos] == '\r') {
        ++p.pos;
        if (p.pos < p.data.size() && p.data[p.pos] == '\n')
            ++p.pos;
    } else if (p.pos < p.data.size() && whitespace(p.data[p.pos]))
        ++p.pos;
    size_t start = p.pos, end = std::string::npos;
    std::string stencil_key;
    auto find_stencil = [&](size_t stop) {
        if (!boolean(get(image, "ImageMask")) || stop - dictionary_start > 16384)
            return false;
        stencil_key = string(p.data.subspan(dictionary_start, stop - dictionary_start));
        auto found = c.renderer->inline_stencils.find(stencil_key);
        if (found == c.renderer->inline_stencils.end())
            return false;
        decoded = found->second;
        if (c.renderer->profiling)
            ++c.renderer->profile.image_hits;
        return true;
    };
    auto remember_stencil = [&] {
        if (stencil_key.empty() || !decoded || decoded->pixels.size() > 65536)
            return;
        auto& r = *c.renderer;
        // Retain small glyph masks only. The key includes the dictionary and
        // encoded bytes, and the cached pixels contain alpha, not the fill color.
        size_t cost = decoded->pixels.size() * 2 + stencil_key.size() + 256;
        if (r.inline_stencil_bytes + cost > 16 * 1024 * 1024 || r.inline_stencils.size() >= 4096) {
            r.inline_stencils.clear();
            r.inline_stencil_bytes = 0;
        }
        if (r.inline_stencils.emplace(stencil_key, decoded).second)
            r.inline_stencil_bytes += cost;
    };
    auto& filter = get(image, "Filter");
    if (filter.kind == Kind::null) {
        int width = integer(get(image, "Width")), height = integer(get(image, "Height")),
            bits = integer(get(image, "BitsPerComponent"), 1);
        auto space = color_space(*c.document, get(image, "ColorSpace"), resources);
        int comps = boolean(get(image, "ImageMask")) ? 1 : space.components;
        require(width > 0 && height > 0 && bits > 0 && bits <= 16, "Invalid inline image dimensions");
        uint64_t size = ((uint64_t(width) * comps * bits + 7) / 8) * height;
        require(size <= p.data.size() - start, "Truncated inline image");
        end = start + size;
    } else {
        std::string_view rest(reinterpret_cast<const char*>(p.data.data()), p.data.size());
        size_t candidate = start;
        unsigned tries = 0;
        while ((candidate = rest.find("EI", candidate)) != std::string_view::npos) {
            if (candidate > start && whitespace(p.data[candidate - 1]) &&
                (candidate + 2 == p.data.size() || delimiter(p.data[candidate + 2]))) {
                size_t stop = candidate - 1;
                if (stop > start && p.data[stop] == '\n' && p.data[stop - 1] == '\r')
                    --stop;
                image.stream = p.data.subspan(start, stop - start);
                image.has_stream = true;
                try {
                    if (find_stencil(stop)) {
                        end = stop;
                        break;
                    }
                    auto stream = decode_stream(*c.document, image, true);
                    if (stream.image_filter.empty()) {
                        end = stop;
                        break;
                    }
                    auto data = load_image(*c.renderer, *c.document, image, resources);
                    if (data) {
                        if (data->stencil) {
                            decoded = std::move(data);
                            remember_stencil();
                        }
                        end = stop;
                        break;
                    }
                } catch (const std::exception&) {
                    if (++tries > 1000)
                        break;
                }
            }
            candidate += 2;
        }
    }
    require(end != std::string::npos, "Unterminated or invalid inline image");
    image.stream = p.data.subspan(start, end - start);
    image.has_stream = true;
    if (!decoded && boolean(get(image, "ImageMask"))) {
        if (!find_stencil(end)) {
            decoded = load_image(*c.renderer, *c.document, image, resources);
            remember_stencil();
        }
    }
    p.pos = end;
    require(token(p) == "EI", "Missing inline image terminator");
    return image;
}

static void interpret(Context& c, View bytes, const Value& resources, int depth) {
    require(depth < 48, "Content recursion limit exceeded");
    Parser p{bytes, 0, 0, false};
    size_t tokens = 0;
    std::vector<Value> operands(8);
    size_t operand_count = 0;
    Value parsed;
    auto next_operand = [&]() -> Value& {
        require(operand_count < 4096, "Content operand limit exceeded");
        if (operand_count == operands.size())
            operands.emplace_back();
        return operands[operand_count++];
    };
    std::vector<Graphics> saved;
    std::vector<Segment> path;
    std::vector<ID2D1Geometry*> text_clips;
    std::vector<bool> marked;
    bool hidden = false;
    int pending_clip = 0, compatibility = 0;
    size_t initial_clips = c.clips.size();
    Point current{}, start{};
    auto arg = [&](size_t i) -> const Value& {
        require(i < operand_count, "Missing content operator operand");
        return operands[i];
    };
    auto n = [&](size_t i) {
        auto& v = arg(i);
        require(v.kind == Kind::number && std::abs(v.number) < 1e12, "Invalid content operator number");
        return v.number;
    };
    auto need = [&](size_t count) {
        require(operand_count == count, "Incorrect content operator operand count");
    };
    auto add = [&](uint8_t type, Point a = {}, Point b = {}, Point cc = {}) {
        require(path.size() < 1000000, "Path segment limit exceeded");
        path.push_back({type, a, b, cc});
    };
    auto end_path = [&](bool fill, bool stroke, bool evenodd, bool closed = false) {
        if (closed) {
            add(3);
            current = start;
        }
        if (!hidden && !c.text_only && (fill || stroke))
            draw_path(c, resources, path, fill, stroke, evenodd);
        if (pending_clip && !hidden && !c.text_only) {
            D2D1_RECT_F bounds{};
            if (axis_rectangle(path, bounds))
                push_clip(c, Clip{nullptr, bounds});
            else {
                auto shape = geometry(c, path, pending_clip == 2);
                push_clip(c, shape);
                release(shape);
            }
        }
        pending_clip = 0;
        path.clear();
    };
    auto line_move = [&](double x, double y) {
        c.g.line = multiply(c.g.line, {1, 0, 0, 1, x, y});
        c.g.text = c.g.line;
    };
    auto set_color = [&](bool stroke, ColorSpace space, std::vector<double> components,
                         std::string pattern = {}) {
        Color value = color(*c.document, space, components);
        if (stroke) {
            c.g.stroke = value;
            c.g.stroke_space = std::move(space);
            c.g.stroke_components = std::move(components);
            c.g.stroke_pattern = std::move(pattern);
        } else {
            c.g.fill = value;
            c.g.fill_space = std::move(space);
            c.g.fill_components = std::move(components);
            c.g.fill_pattern = std::move(pattern);
        }
    };
    try {
        while (p.pos < bytes.size()) {
            skip_space(p);
            if (p.pos == bytes.size())
                break;
            if ((++tokens & 255) == 0)
                check_cancel(*c.document);
            ProfileTimer parse_timer(c.renderer->profiling ? &c.renderer->profile.parse_ms : nullptr);
            size_t token_start = p.pos;
            auto first = p.data[p.pos];
            if ((first >= '0' && first <= '9') || first == '.' || first == '+' || first == '-') {
                auto text = token(p);
                if (!text.empty() && text.front() == '+')
                    text.remove_prefix(1);
                double number = 0;
                auto result = std::from_chars(text.data(), text.data() + text.size(), number);
                if (result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(number)) {
                    auto& operand = next_operand();
                    operand.kind = Kind::number;
                    operand.number = number;
                    continue;
                }
                p.pos = token_start;
            }
            std::string_view op;
            if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '\'' || first == '"') {
                op = token(p);
                if (op == "true" || op == "false" || op == "null") {
                    op = {};
                    p.pos = token_start;
                }
            }
            if (op.empty()) {
                parsed = parse(p);
                if (parsed.kind != Kind::keyword) {
                    next_operand() = std::move(parsed);
                    continue;
                }
                op = parsed.text;
            }
            parse_timer.stop();
            require(++c.operations < 10000000, "Page operator limit exceeded");
            if (op == "q") {
                need(0);
                require(saved.size() < 256, "Graphics state stack limit exceeded");
                c.g.clips = c.clips.size();
                saved.push_back(c.g);
            } else if (op == "Q") {
                need(0);
                if (!saved.empty()) {
                    pop_clips(c, saved.back().clips);
                    c.g = std::move(saved.back());
                    saved.pop_back();
                } else
                    warn(*c.document, "Unbalanced graphics-state restore ignored");
            } else if (op == "cm") {
                need(6);
                Matrix m{n(0), n(1), n(2), n(3), n(4), n(5)};
                c.g.ctm = multiply(c.g.ctm, m);
            } else if (op == "w") {
                need(1);
                c.g.width = n(0);
            } else if (op == "J") {
                need(1);
                c.g.cap = int(n(0));
            } else if (op == "j") {
                need(1);
                c.g.join = int(n(0));
            } else if (op == "M") {
                need(1);
                c.g.miter = n(0);
            } else if (op == "d") {
                need(2);
                c.g.dash.clear();
                require(arg(0).kind == Kind::array && arg(0).array.size() <= 1024, "Invalid dash array");
                double total = 0;
                for (auto& v : arg(0).array) {
                    double length = num(v, -1);
                    require(length >= 0, "Negative dash length");
                    c.g.dash.push_back(float(length));
                    total += length;
                }
                if (total == 0)
                    c.g.dash.clear();
                c.g.dash_offset = n(1);
            } else if (op == "gs") {
                need(1);
                apply_ext(c, entry(*c.document, entry(*c.document, resources, "ExtGState"), arg(0).text),
                          resources);
            } else if (op == "m") {
                need(2);
                current = start = transform(c.g.ctm, {n(0), n(1)});
                add(0, current);
            } else if (op == "l") {
                need(2);
                current = transform(c.g.ctm, {n(0), n(1)});
                add(1, current);
            } else if (op == "c") {
                need(6);
                Point a = transform(c.g.ctm, {n(0), n(1)}), b = transform(c.g.ctm, {n(2), n(3)});
                current = transform(c.g.ctm, {n(4), n(5)});
                add(2, a, b, current);
            } else if (op == "v") {
                need(4);
                Point a = current, b = transform(c.g.ctm, {n(0), n(1)});
                current = transform(c.g.ctm, {n(2), n(3)});
                add(2, a, b, current);
            } else if (op == "y") {
                need(4);
                Point a = transform(c.g.ctm, {n(0), n(1)});
                current = transform(c.g.ctm, {n(2), n(3)});
                add(2, a, current, current);
            } else if (op == "h") {
                need(0);
                add(3);
                current = start;
            } else if (op == "re") {
                need(4);
                double x = n(0), y = n(1), w = n(2), h = n(3);
                current = start = transform(c.g.ctm, {x, y});
                add(0, current);
                add(1, transform(c.g.ctm, {x + w, y}));
                add(1, transform(c.g.ctm, {x + w, y + h}));
                add(1, transform(c.g.ctm, {x, y + h}));
                add(3);
            } else if (op == "S" || op == "s") {
                need(0);
                end_path(false, true, false, op == "s");
            } else if (op == "f" || op == "F" || op == "f*") {
                need(0);
                end_path(true, false, op == "f*");
            } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
                need(0);
                end_path(true, true, op.ends_with('*'), op[0] == 'b');
            } else if (op == "n") {
                need(0);
                end_path(false, false, false);
            } else if (op == "W" || op == "W*") {
                need(0);
                pending_clip = op == "W" ? 1 : 2;
            } else if (op == "g" || op == "G" || op == "rg" || op == "RG" || op == "k" || op == "K") {
                bool stroke = op[0] >= 'A' && op[0] <= 'Z';
                size_t count = op == "g" || op == "G" ? 1 : op == "rg" || op == "RG" ? 3 : 4;
                need(count);
                ColorSpace cs;
                cs.name = count == 1 ? "DeviceGray" : count == 3 ? "DeviceRGB" : "DeviceCMYK";
                cs.components = int(count);
                std::vector<double> values;
                for (size_t i = 0; i < count; ++i)
                    values.push_back(n(i));
                set_color(stroke, cs, std::move(values));
            } else if (op == "cs" || op == "CS") {
                need(1);
                auto cs = color_space(*c.document, arg(0), resources);
                std::vector<double> initial(cs.components, 0);
                if (cs.name == "DeviceCMYK")
                    initial[3] = 1;
                if (cs.name == "Separation" || cs.name == "DeviceN")
                    std::fill(initial.begin(), initial.end(), 1);
                set_color(op == "CS", cs, std::move(initial));
            } else if (op == "sc" || op == "SC" || op == "scn" || op == "SCN") {
                bool stroke = op[0] == 'S';
                auto space = stroke ? c.g.stroke_space : c.g.fill_space;
                std::string pattern;
                std::vector<double> values;
                for (auto& v : std::span(operands).first(operand_count)) {
                    if (v.kind == Kind::name)
                        pattern = v.text;
                    else {
                        require(v.kind == Kind::number, "Invalid color operand");
                        values.push_back(v.number);
                    }
                }
                set_color(stroke, space, std::move(values), std::move(pattern));
            } else if (op == "BT") {
                need(0);
                c.g.text = c.g.line = {};
            } else if (op == "ET") {
                need(0);
                finish_text_clip(c, text_clips);
            } else if (op == "Tf") {
                need(2);
                auto& font = entry(*c.document, entry(*c.document, resources, "Font"), arg(0).text);
                c.g.font = &load_font(*c.renderer, *c.document, font);
                c.g.font_size = n(1);
            } else if (op == "Tc") {
                need(1);
                c.g.char_space = n(0);
            } else if (op == "Tw") {
                need(1);
                c.g.word_space = n(0);
            } else if (op == "Tz") {
                need(1);
                c.g.hscale = n(0) / 100;
            } else if (op == "TL") {
                need(1);
                c.g.leading = n(0);
            } else if (op == "Ts") {
                need(1);
                c.g.rise = n(0);
            } else if (op == "Tr") {
                need(1);
                c.g.text_mode = int(n(0));
            } else if (op == "Tm") {
                need(6);
                c.g.text = c.g.line = {n(0), n(1), n(2), n(3), n(4), n(5)};
            } else if (op == "Td" || op == "TD") {
                need(2);
                if (op == "TD")
                    c.g.leading = -n(1);
                line_move(n(0), n(1));
            } else if (op == "T*") {
                need(0);
                line_move(0, -c.g.leading);
            } else if (op == "Tj" || op == "'" || op == "\"") {
                need(op == "\"" ? 3 : 1);
                if (op == "\"") {
                    c.g.word_space = n(0);
                    c.g.char_space = n(1);
                }
                if (op != "Tj")
                    line_move(0, -c.g.leading);
                if (!hidden)
                    show_text(c, arg(op == "\"" ? 2 : 0).text, resources, text_clips);
            } else if (op == "TJ") {
                need(1);
                require(arg(0).kind == Kind::array, "Invalid TJ text array");
                for (auto& item : arg(0).array) {
                    if (item.kind == Kind::string) {
                        if (!hidden)
                            show_text(c, item.text, resources, text_clips);
                    } else if (item.kind == Kind::number) {
                        double advance = -item.number * c.g.font_size / 1000;
                        c.g.text = multiply(c.g.text, c.g.font && c.g.font->cmap.vertical
                                                          ? Matrix{1, 0, 0, 1, 0, advance}
                                                          : Matrix{1, 0, 0, 1, advance * c.g.hscale, 0});
                    }
                }
            } else if (op == "Do") {
                need(1);
                if (!hidden) {
                    auto& object = entry(*c.document, entry(*c.document, resources, "XObject"), arg(0).text);
                    if (visible(*c.document, get(object, "OC"))) {
                        try {
                            if (named(get(object, "Subtype"), "Image")) {
                                if (!c.text_only)
                                    draw_image(c, object, resources);
                            } else if (named(get(object, "Subtype"), "Form"))
                                draw_form(c, object, resources, depth + 1);
                            else
                                warn(*c.document,
                                     "Unsupported XObject subtype: " + get(object, "Subtype").text);
                        } catch (const std::exception& e) {
                            check_cancel(*c.document);
                            warn(*c.document, std::string("XObject could not be drawn: ") + e.what());
                        }
                    }
                }
            } else if (op == "sh") {
                need(1);
                // A collapsed shading has no area and no inverse coordinates.
                if (!hidden && !c.text_only && std::abs(c.g.ctm.a * c.g.ctm.d - c.g.ctm.b * c.g.ctm.c) > 1e-16) {
                    auto& shading = entry(*c.document, entry(*c.document, resources, "Shading"), arg(0).text);
                    paint(c, resources, [&](Context& target) {
                        Rect region;
                        auto image = shading_image(target, shading, resources, &region);
                        if (region.x1 <= region.x0 || region.y1 <= region.y0)
                            return;
                        auto brush = bitmap_brush(
                            target, *image, {1, 0, 0, 1, region.x0, region.y0},
                            target.g.fill_alpha, false);
                        set_transform(target);
                        target.canvas->target->FillRectangle(
                            D2D1::RectF(float(region.x0), float(region.y0), float(region.x1), float(region.y1)),
                            brush);
                        release(brush);
                    });
                }
            } else if (op == "BI") {
                need(0);
                std::shared_ptr<Image> decoded;
                auto image = inline_image(c, p, resources, decoded);
                if (!hidden && !c.text_only)
                    draw_image(c, image, resources, std::move(decoded));
            } else if (op == "BMC" || op == "BDC") {
                require(marked.size() < 128, "Marked-content nesting limit");
                marked.push_back(hidden);
                if (op == "BDC" && operand_count == 2 && named(arg(0), "OC")) {
                    const Value& property =
                        arg(1).kind == Kind::name
                            ? get(entry(*c.document, resources, "Properties"), arg(1).text)
                            : arg(1);
                    hidden = hidden || !visible(*c.document, property);
                }
            } else if (op == "EMC") {
                if (!marked.empty()) {
                    hidden = marked.back();
                    marked.pop_back();
                }
            } else if (op == "BX")
                ++compatibility;
            else if (op == "EX")
                compatibility = std::max(0, compatibility - 1);
            else if (op == "ri" || op == "i" || op == "MP" || op == "DP" || op == "d0" || op == "d1") {
            } else if (!compatibility)
                warn(*c.document, "Unknown content operator: " + std::string(op));
            // Retain the small numeric slots used by dense vector content.
            // Release complex operands after each operator.
            for (auto& operand : std::span(operands).first(operand_count))
                if (operand.kind != Kind::number)
                    operand = {};
            operand_count = 0;
            // Content operands have now been consumed. Keep the allocation
            // limit per live operand set; the separate operation budget bounds
            // long streams without rejecting millions of already-freed values.
            p.nodes = 0;
        }
        finish_text_clip(c, text_clips);
        if (!saved.empty()) {
            pop_clips(c, initial_clips);
            c.g = saved.front();
            warn(*c.document, "Unbalanced saved graphics state at end of content");
        }
    } catch (...) {
        for (auto shape : text_clips)
            release(shape);
        throw;
    }
}
static const Value& field_entry(Document& d, const Value& field, std::string_view key) {
    const Value* current = &field;
    for (int depth = 0; depth < 32 && current->kind == Kind::dict; ++depth) {
        auto& value = entry(d, *current, key);
        if (value.kind != Kind::null)
            return value;
        current = &entry(d, *current, "Parent");
    }
    static const Value missing;
    return missing;
}
static Color annotation_color(Document& d, const Value& value, Color fallback) {
    if (value.array.empty())
        return fallback;
    ColorSpace s;
    s.components = int(value.array.size());
    s.name = s.components == 4 ? "DeviceCMYK" : s.components == 3 ? "DeviceRGB" : "DeviceGray";
    std::vector<double> v;
    for (auto& item : value.array)
        v.push_back(num(item));
    return color(d, s, v);
}
static void widget_appearance(Context& c, const Value& widget, Rect box, Matrix page_matrix,
                              const Value& resources) {
    auto& d = *c.document;
    auto& form = entry(d, d.catalog, "AcroForm");
    auto& kind = field_entry(d, widget, "FT");
    int flags = integer(field_entry(d, widget, "Ff"));
    auto& value = field_entry(d, widget, "V");
    auto& mk = entry(d, widget, "MK");
    auto saved = c.g;
    c.g.ctm = page_matrix;
    Color background = annotation_color(d, entry(d, mk, "BG"), {1, 1, 1, 0}),
          border = annotation_color(d, entry(d, mk, "BC"), {0, 0, 0, 0}), foreground{0, 0, 0, 1};
    double font_size = 0;
    auto& da = field_entry(d, widget, "DA");
    std::string appearance = da.kind == Kind::string ? da.text : get(form, "DA").text;
    if (!appearance.empty()) {
        Parser parser{view(appearance)};
        parser.references = false;
        std::vector<Value> operands;
        while (parser.pos < parser.data.size()) {
            auto item = parse(parser);
            if (item.kind != Kind::keyword)
                operands.push_back(std::move(item));
            else {
                if (item.text == "Tf" && operands.size() >= 2)
                    font_size = num(operands.back());
                else if (item.text == "g" && operands.size() >= 1)
                    foreground = {float(num(operands.back())), float(num(operands.back())),
                                  float(num(operands.back())), 1};
                else if (item.text == "rg" && operands.size() >= 3)
                    foreground = {float(num(operands[operands.size() - 3])),
                                  float(num(operands[operands.size() - 2])), float(num(operands.back())), 1};
                operands.clear();
            }
            require(operands.size() < 64, "Invalid widget default appearance");
        }
    }
    double width = box.x1 - box.x0, height = box.y1 - box.y0;
    if (width <= 0 || height <= 0) {
        c.g = saved;
        return;
    }
    std::wstring text = pdf_text(value.text);
    if (value.kind == Kind::array) {
        text.clear();
        for (auto& item : value.array) {
            if (!text.empty())
                text += L"\n";
            text += pdf_text(resolve(d, item).text);
        }
    }
    bool button = named(kind, "Btn"), push = button && (flags & (1 << 16)),
         radio = button && (flags & (1 << 15));
    if (push)
        text = pdf_text(get(mk, "CA").text);
    else if (button)
        text.clear();
    if (flags & (1 << 13))
        text.assign(text.size(), L'\x2022');
    if (named(kind, "Sig")) {
        text.clear();
        if (value.kind != Kind::null)
            warn(d, "A signature field has no stored appearance; signature verification is not available");
    }
    if (c.text_only) {
        if (!text.empty())
            c.output->text.push_back({text, transformed_box(page_matrix, box)});
        c.g = saved;
        return;
    }
    if (font_size <= 0)
        font_size = std::min(12.0, std::max(1.0, (height - 4) * 0.8));
    paint(
        c, resources,
        [&](Context& target) {
            auto* rt = target.canvas->target;
            set_transform(target, multiply(page_matrix, {1, 0, 0, -1, box.x0, box.y1}));
            ID2D1SolidColorBrush* brush = nullptr;
            IDWriteTextFormat* format = nullptr;
            try {
                hr(rt->CreateSolidColorBrush(rgba(background), &brush), "Create form appearance brush");
                auto rect = D2D1::RectF(0, 0, float(width), float(height));
                if (background.a > 0)
                    rt->FillRectangle(rect, brush);
                if (border.a > 0) {
                    brush->SetColor(rgba(border));
                    float stroke = float(std::max(0.0, num(entry(d, entry(d, widget, "BS"), "W"), 1)));
                    if (stroke > 0)
                        rt->DrawRectangle(D2D1::RectF(stroke / 2, stroke / 2, float(width) - stroke / 2,
                                                      float(height) - stroke / 2),
                                          brush, stroke);
                }
                brush->SetColor(rgba(foreground));
                if (button && !push && value.kind != Kind::null && !value.text.empty() &&
                    value.text != "Off") {
                    if (radio)
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(float(width / 2), float(height / 2)),
                                                      float(width * 0.24), float(height * 0.24)),
                                        brush);
                    else {
                        rt->DrawLine(D2D1::Point2F(float(width * 0.2), float(height * 0.5)),
                                     D2D1::Point2F(float(width * 0.43), float(height * 0.75)), brush,
                                     float(std::max(1.0, std::min(width, height) * 0.1)));
                        rt->DrawLine(D2D1::Point2F(float(width * 0.43), float(height * 0.75)),
                                     D2D1::Point2F(float(width * 0.82), float(height * 0.2)), brush,
                                     float(std::max(1.0, std::min(width, height) * 0.1)));
                    }
                } else if (!text.empty()) {
                    hr(target.renderer->write->CreateTextFormat(
                           L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, float(font_size), L"", &format),
                       "Create form text format");
                    int alignment = integer(field_entry(d, widget, "Q"), integer(get(form, "Q")));
                    format->SetTextAlignment(alignment == 1   ? DWRITE_TEXT_ALIGNMENT_CENTER
                                             : alignment == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
                    format->SetParagraphAlignment((flags & (1 << 12)) ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                                                      : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    format->SetWordWrapping((flags & (1 << 12)) ? DWRITE_WORD_WRAPPING_WRAP
                                                                : DWRITE_WORD_WRAPPING_NO_WRAP);
                    auto text_rect =
                        D2D1::RectF(2, 1, float(std::max(2.0, width - 2)), float(std::max(1.0, height - 1)));
                    rt->DrawTextW(text.data(), UINT32(text.size()), format, text_rect, brush,
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            } catch (...) {
                release(format);
                release(brush);
                throw;
            }
            release(format);
            release(brush);
        },
        false, 1, transformed_box(page_matrix, box));
    if (!text.empty())
        c.output->text.push_back({text, transformed_box(page_matrix, box)});
    c.g = saved;
}
static void annotations(Context& c, const Page& page, const Value& resources, Matrix page_matrix) {
    auto& d = *c.document;
    auto& list = resolve(d, page.annotations);
    for (auto& ref : list.array) {
        auto& annot = resolve(d, ref);
        int flags = integer(get(annot, "F"));
        if (flags & (1 | 2 | 32))
            continue;
        Rect box = rectangle(entry(d, annot, "Rect"));
        if (!visible(d, get(annot, "OC")))
            continue;
        if (named(get(annot, "Subtype"), "Link")) {
            auto& action = entry(d, annot, "A");
            int dest = destination_page(d, get(annot, "Dest").kind != Kind::null ? get(annot, "Dest")
                                                                                 : get(action, "D"));
            std::wstring uri = named(get(action, "S"), "URI") ? pdf_text(get(action, "URI").text) : L"";
            c.output->links.push_back({transformed_box(page_matrix, box), dest, uri});
        }
        auto& normal = entry(d, entry(d, annot, "AP"), "N");
        const Value* appearance = &normal;
        if (!normal.has_stream && normal.kind == Kind::dict)
            appearance = &entry(d, normal, get(annot, "AS").text);
        if (appearance->has_stream) {
            auto saved = c.g;
            try {
                Rect bounds = rectangle(entry(d, *appearance, "BBox"));
                Matrix m = value_matrix(entry(d, *appearance, "Matrix"));
                bounds = transformed_box(m, bounds);
                require(bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0, "Empty annotation appearance");
                double sx = (box.x1 - box.x0) / (bounds.x1 - bounds.x0),
                       sy = (box.y1 - box.y0) / (bounds.y1 - bounds.y0);
                c.g.ctm =
                    multiply(page_matrix, {sx, 0, 0, sy, box.x0 - bounds.x0 * sx, box.y0 - bounds.y0 * sy});
                draw_form(c, *appearance, resources, 1);
            } catch (const std::exception& e) {
                check_cancel(d);
                warn(d, std::string("Annotation appearance failed: ") + e.what());
            }
            c.g = saved;
        } else if (!c.text_only &&
                   (named(get(annot, "Subtype"), "Highlight") || named(get(annot, "Subtype"), "Underline") ||
                    named(get(annot, "Subtype"), "StrikeOut") || named(get(annot, "Subtype"), "Square"))) {
            auto saved = c.g;
            c.g.ctm = page_matrix;
            c.g.fill = c.g.stroke = {1, 0.85f, 0.1f, 1};
            auto& value = entry(d, annot, "C");
            if (value.array.size() >= 3)
                c.g.fill = c.g.stroke = {float(num(at(value, 0))), float(num(at(value, 1))),
                                         float(num(at(value, 2))), 1};
            c.g.fill_alpha = 0.3f;
            c.g.width = 1;
            bool fill = named(get(annot, "Subtype"), "Highlight"),
                 square = named(get(annot, "Subtype"), "Square");
            std::vector<Segment> path;
            if (fill || square)
                path = {{0, transform(page_matrix, {box.x0, box.y0}), {}, {}},
                        {1, transform(page_matrix, {box.x1, box.y0}), {}, {}},
                        {1, transform(page_matrix, {box.x1, box.y1}), {}, {}},
                        {1, transform(page_matrix, {box.x0, box.y1}), {}, {}},
                        {3, {}, {}, {}}};
            else {
                double y = named(get(annot, "Subtype"), "StrikeOut") ? (box.y0 + box.y1) / 2 : box.y0;
                path = {{0, transform(page_matrix, {box.x0, y}), {}, {}},
                        {1, transform(page_matrix, {box.x1, y}), {}, {}}};
            }
            draw_path(c, resources, path, fill, !fill, false);
            c.g = saved;
        } else if (named(get(annot, "Subtype"), "Widget")) {
            auto saved = c.g;
            try {
                widget_appearance(c, annot, box, page_matrix, resources);
            } catch (const std::exception& e) {
                check_cancel(d);
                warn(d, std::string("Form appearance could not be generated: ") + e.what());
            }
            c.g = saved;
        }
    }
}
RenderedPage render_page(Renderer& r, Document& d, size_t page_index, double scale, int extra_rotation,
                         bool text_only) {
    require(page_index < d.pages.size(), "Page index out of range");
    require(scale > 0 && std::isfinite(scale), "Invalid render scale");
    check_cancel(d);
    auto begin = std::chrono::steady_clock::now();
    if (r.profiling)
        r.profile = {};
    auto& page = d.pages[page_index];
    scale *= page.user_unit;
    auto b = page.box;
    double w = (b.x1 - b.x0) * scale, h = (b.y1 - b.y0) * scale;
    int rotation = ((page.rotation + extra_rotation) % 360 + 360) % 360;
    require(rotation % 90 == 0, "Rotation must be a multiple of 90 degrees");
    Matrix m{scale, 0, 0, -scale, -b.x0 * scale, b.y1 * scale};
    if (rotation == 90) {
        m = multiply({0, 1, -1, 0, h, 0}, m);
        std::swap(w, h);
    } else if (rotation == 180)
        m = multiply({-1, 0, 0, -1, w, h}, m);
    else if (rotation == 270) {
        m = multiply({0, -1, 1, 0, 0, w}, m);
        std::swap(w, h);
    }
    require(w > 0 && h > 0 && w <= 32768 && h <= 32768 && std::ceil(w) * std::ceil(h) <= max_stream / 4,
            "Rendered page exceeds the 64-megapixel limit; reduce zoom");
    RenderedPage out;
    out.width = unsigned(std::ceil(w));
    out.height = unsigned(std::ceil(h));
    out.page_matrix = m;
    ProfileTimer surface_timer(r.profiling ? &r.profile.surface_ms : nullptr);
    auto target = surface(r, text_only ? 1 : out.width, text_only ? 1 : out.height);
    surface_timer.stop();
    Context c;
    c.renderer = &r;
    c.document = &d;
    c.canvas = target.get();
    c.output = &out;
    c.g.ctm = m;
    c.text_only = text_only;
    target->target->BeginDraw();
    // The page is an isolated transparency group. Composite the completed
    // page onto white only after its internal blend modes have been applied.
    target->target->Clear(D2D1::ColorF(0, 0));
    try {
        ProfileTimer content_timer(r.profiling ? &r.profile.content_ms : nullptr);
        auto content = page_content(d, page);
        content_timer.stop();
        ProfileTimer interpret_timer(r.profiling ? &r.profile.interpret_ms : nullptr);
        interpret(c, content, resolve(d, page.resources), 0);
        pop_clips(c, 0);
        c.g = {};
        c.g.ctm = m;
        annotations(c, page, resolve(d, page.resources), m);
        pop_clips(c, 0);
        interpret_timer.stop();
        ProfileTimer finish_timer(r.profiling ? &r.profile.finish_ms : nullptr);
        hr(target->target->EndDraw(), "Render PDF page");
        finish_timer.stop();
        if (!text_only) {
            ProfileTimer readback_timer(r.profiling ? &r.profile.readback_ms : nullptr);
            out.pixels = pixels(*target);
            for (size_t i = 0; i < out.pixels.size(); i += 4) {
                unsigned paper = 255 - out.pixels[i + 3];
                for (unsigned channel = 0; channel < 3; ++channel)
                    out.pixels[i + channel] = uint8_t(std::min(255u, out.pixels[i + channel] + paper));
                out.pixels[i + 3] = 255;
            }
        }
    } catch (...) {
        pop_clips(c, 0);
        target->target->EndDraw();
        throw;
    }
    out.warnings = d.warnings;
    out.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return out;
}
std::wstring page_text(const RenderedPage& page) {
    std::wstring result;
    const TextSpan* previous = nullptr;
    for (auto& span : page.text) {
        if (previous && !result.empty()) {
            double h = std::max(1.0, previous->bounds.y1 - previous->bounds.y0);
            if (std::abs(span.bounds.y0 - previous->bounds.y0) > h * 0.55)
                result += '\n';
            else if (span.bounds.x0 - previous->bounds.x1 > h * 0.12 && result.back() != ' ')
                result += ' ';
        }
        result += span.text;
        previous = &span;
    }
    return result;
}
} // namespace pdf
