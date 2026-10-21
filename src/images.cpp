#include "render.h"
#include "image_codecs.h"

namespace pdf {
ColorSpace color_space(Document& d, const Value& input, const Value& resources, int depth) {
    require(depth < 16, "Color space recursion limit exceeded");
    const auto& v = resolve(d, input);
    ColorSpace s;
    s.definition = v;
    if (v.kind == Kind::name) {
        s.name = v.text;
        if (s.name == "G")
            s.name = "DeviceGray";
        if (s.name == "RGB")
            s.name = "DeviceRGB";
        if (s.name == "CMYK")
            s.name = "DeviceCMYK";
        if (s.name != "DeviceGray" && s.name != "DeviceRGB" && s.name != "DeviceCMYK" &&
            s.name != "Pattern") {
            auto& defined = entry(d, entry(d, resources, "ColorSpace"), s.name);
            require(defined.kind != Kind::null, "Unknown color space: " + s.name);
            return color_space(d, defined, resources, depth + 1);
        }
    } else if (v.kind == Kind::array)
        s.name = at(v, 0).text;
    if (s.name == "DeviceRGB" || s.name == "CalRGB" || s.name == "Lab")
        s.components = 3;
    else if (s.name == "DeviceCMYK")
        s.components = 4;
    else if (s.name == "ICCBased")
        s.components = integer(entry(d, at(v, 1), "N"), 3);
    else if (s.name == "DeviceN")
        s.components = int(resolve(d, at(v, 1)).array.size());
    else if (s.name == "Pattern" && v.array.size() > 1)
        s.components = color_space(d, at(v, 1), resources, depth + 1).components;
    require(s.components >= 1 && s.components <= 32, "Invalid color component count");
    if (s.name == "Indexed" || s.name == "I") {
        s.alternate = std::make_shared<ColorSpace>(color_space(d, at(v, 1), resources, depth + 1));
        auto& lookup = resolve(d, at(v, 3));
        if (lookup.has_stream)
            s.lookup = decode_stream(d, lookup).bytes;
        else
            s.lookup.assign(lookup.text.begin(), lookup.text.end());
    } else if (s.name == "Separation" || s.name == "DeviceN") {
        s.alternate = std::make_shared<ColorSpace>(color_space(d, at(v, 2), resources, depth + 1));
        s.function = at(v, 3);
    } else if (s.name == "ICCBased") {
        auto& profile = resolve(d, at(v, 1));
        auto& alt = get(profile, "Alternate");
        if (alt.kind != Kind::null)
            s.alternate = std::make_shared<ColorSpace>(color_space(d, alt, resources, depth + 1));
        else {
            s.alternate = std::make_shared<ColorSpace>();
            s.alternate->components = s.components;
            s.alternate->name = s.components == 4   ? "DeviceCMYK"
                                : s.components == 3 ? "DeviceRGB"
                                                    : "DeviceGray";
        }
    } else if (s.name == "Pattern" && v.array.size() > 1)
        s.alternate = std::make_shared<ColorSpace>(color_space(d, at(v, 1), resources, depth + 1));
    return s;
}
static float unit(double v) {
    return float(std::clamp(v, 0.0, 1.0));
}
static Color xyz(double x, double y, double z) {
    auto gamma = [](double v) {
        return unit(v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055);
    };
    return {gamma(3.2406 * x - 1.5372 * y - 0.4986 * z), gamma(-0.9689 * x + 1.8758 * y + 0.0415 * z),
            gamma(0.0557 * x - 0.2040 * y + 1.0570 * z), 1};
}
Color color(Document& d, const ColorSpace& s, std::span<const double> values, int depth) {
    require(depth < 16, "Color conversion recursion limit exceeded");
    auto v = [&](size_t i, double fallback = 0) { return i < values.size() ? values[i] : fallback; };
    auto& def = s.definition;
    if (s.name == "DeviceRGB")
        return {unit(v(0)), unit(v(1)), unit(v(2)), 1};
    if (s.name == "DeviceCMYK") {
        double k = v(3);
        return {unit(1 - std::min(1.0, v(0) + k)), unit(1 - std::min(1.0, v(1) + k)),
                unit(1 - std::min(1.0, v(2) + k)), 1};
    }
    if (s.name == "Indexed" || s.name == "I") {
        require(bool(s.alternate), "Missing indexed color space");
        auto& base = *s.alternate;
        int high = integer(at(def, 2), 255);
        int index = int(std::clamp(v(0), 0.0, double(std::max(0, high))));
        View table = s.lookup;
        std::array<double, 32> components{};
        for (int i = 0; i < base.components; ++i) {
            size_t pos = size_t(index) * base.components + i;
            components[i] = pos < table.size() ? table[pos] / 255.0 : 0;
        }
        return color(d, base, std::span(components).first(base.components), depth + 1);
    }
    if (s.name == "Separation" || s.name == "DeviceN") {
        require(bool(s.alternate), "Missing alternate color space");
        auto converted = evaluate_function(d, s.function, values, depth + 1);
        return color(d, *s.alternate, converted, depth + 1);
    }
    if (s.name == "ICCBased") {
        require(bool(s.alternate), "Missing ICC alternate color space");
        return color(d, *s.alternate, values, depth + 1);
    }
    if (s.name == "CalGray" || s.name == "CalRGB" || s.name == "Lab") {
        auto& params = resolve(d, at(def, 1));
        auto& white = entry(d, params, "WhitePoint");
        double wx = num(at(white, 0), 0.95047), wz = num(at(white, 2), 1.08883);
        if (s.name == "CalGray") {
            double y = std::pow(std::max(0.0, v(0)), num(get(params, "Gamma"), 1));
            return xyz(y * wx, y, y * wz);
        }
        if (s.name == "Lab") {
            auto f = [](double x) { return x >= 6.0 / 29 ? x * x * x : (108.0 / 841) * (x - 4.0 / 29); };
            double m = (v(0) + 16) / 116;
            return xyz(wx * f(m + v(1) / 500), f(m), wz * f(m - v(2) / 200));
        }
        auto& gamma = entry(d, params, "Gamma");
        double a = std::pow(std::max(0.0, v(0)), num(at(gamma, 0), 1)),
               b = std::pow(std::max(0.0, v(1)), num(at(gamma, 1), 1)),
               c = std::pow(std::max(0.0, v(2)), num(at(gamma, 2), 1));
        auto& matrix = entry(d, params, "Matrix");
        if (matrix.array.size() == 9)
            return xyz(a * num(at(matrix, 0)) + b * num(at(matrix, 3)) + c * num(at(matrix, 6)),
                       a * num(at(matrix, 1)) + b * num(at(matrix, 4)) + c * num(at(matrix, 7)),
                       a * num(at(matrix, 2)) + b * num(at(matrix, 5)) + c * num(at(matrix, 8)));
        return xyz(a, b, c);
    }
    if (s.name == "Pattern" && s.alternate)
        return color(d, *s.alternate, values, depth + 1);
    return {unit(v(0)), unit(v(0)), unit(v(0)), 1};
}
static void little(Bytes& b, uint32_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        b.push_back(uint8_t(n >> (i * 8)));
}
static Bytes fax_tiff(View bytes, unsigned width, unsigned height, const Value& params) {
    int k = integer(get(params, "K"));
    bool eol = boolean(get(params, "EndOfLine"));
    uint16_t compression = k < 0 ? 4 : k > 0 || eol ? 3 : 2;
    Bytes out{'I', 'I', 42, 0, 8, 0, 0, 0};
    constexpr uint16_t entries = 11;
    little(out, entries, 2);
    uint32_t offset = 8 + 2 + entries * 12 + 4;
    auto tag = [&](uint16_t id, uint16_t type, uint32_t count, uint32_t value) {
        little(out, id, 2);
        little(out, type, 2);
        little(out, count, 4);
        little(out, value, 4);
    };
    tag(256, 4, 1, width);
    tag(257, 4, 1, height);
    tag(258, 3, 1, 1);
    tag(259, 3, 1, compression);
    tag(262, 3, 1, 0);
    tag(266, 3, 1, 1);
    tag(273, 4, 1, offset);
    tag(277, 3, 1, 1);
    tag(278, 4, 1, height);
    tag(279, 4, 1, uint32_t(bytes.size()));
    tag(compression == 4 ? 293 : 292, 4, 1,
        (k > 0 ? 1u : 0u) | (boolean(get(params, "EncodedByteAlign")) ? 4u : 0u));
    little(out, 0, 4);
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}
static Image wic_decode(Renderer& r, View data, unsigned reduction = 0, unsigned expected_width = 0,
                        unsigned expected_height = 0, bool cmyk = false) {
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    Image out;
    try {
        require(data.size() <= UINT32_MAX, "Image codec input limit exceeded");
        hr(r.wic->CreateStream(&stream), "Create image input stream");
        hr(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), DWORD(data.size())),
           "Initialize image input stream");
        hr(r.wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
           "Decode image (codec unavailable or invalid data)");
        hr(decoder->GetFrame(0, &frame), "Read image frame");
        hr(frame->GetSize(&out.width, &out.height), "Read image dimensions");
        require(out.width > 0 && out.height > 0 && uint64_t(out.width) * out.height <= max_stream / 4,
                "Decoded image limit exceeded");
        require((!expected_width || out.width == expected_width) &&
                    (!expected_height || out.height == expected_height),
                "Image codec dimensions disagree with the PDF dictionary");
        IWICBitmapSource* source = frame;
        if (reduction) {
            unsigned divisor = 1u << reduction;
            out.width = (out.width + divisor - 1) / divisor;
            out.height = (out.height + divisor - 1) / divisor;
            out.reduction = reduction;
            hr(r.wic->CreateBitmapScaler(&scaler), "Create image scaler");
            // WIC pushes scaling into the JPEG decoder's native DCT downscale.
            hr(scaler->Initialize(frame, out.width, out.height, WICBitmapInterpolationModeFant),
               "Scale decoded image");
            source = scaler;
        }
        hr(r.wic->CreateFormatConverter(&converter), "Create image converter");
        hr(converter->Initialize(source, cmyk ? GUID_WICPixelFormat32bppCMYK : GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom),
           "Convert image pixels");
        out.pixels.resize(size_t(out.width) * out.height * 4);
        hr(converter->CopyPixels(nullptr, out.width * 4, UINT(out.pixels.size()), out.pixels.data()),
           "Copy decoded pixels");
    } catch (...) {
        release(converter);
        release(scaler);
        release(frame);
        release(decoder);
        release(stream);
        throw;
    }
    release(converter);
    release(scaler);
    release(frame);
    release(decoder);
    release(stream);
    return out;
}
std::shared_ptr<Image> image_level(Document& d, std::shared_ptr<Image> source, double width, double height) {
    width = std::max(1.0, width);
    height = std::max(1.0, height);
    while (source->width > width * 2 && source->height > height * 2) {
        if (!source->half) {
            auto half = std::make_shared<Image>();
            half->width = (source->width + 1) / 2;
            half->height = (source->height + 1) / 2;
            half->stencil = source->stencil;
            half->reduction = source->reduction + 1;
            half->pixels.resize(size_t(half->width) * half->height * 4);
            for (unsigned y = 0; y < half->height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                const auto* row0 = source->pixels.data() + size_t(y * 2) * source->width * 4;
                const auto* row1 = source->pixels.data() +
                                   size_t(std::min(y * 2 + 1, source->height - 1)) * source->width * 4;
                for (unsigned x = 0; x < half->width; ++x) {
                    unsigned x0 = x * 2, x1 = std::min(x0 + 1, source->width - 1);
                    uint32_t a, b, c, e;
                    std::memcpy(&a, row0 + size_t(x0) * 4, 4);
                    std::memcpy(&b, row0 + size_t(x1) * 4, 4);
                    std::memcpy(&c, row1 + size_t(x0) * 4, 4);
                    std::memcpy(&e, row1 + size_t(x1) * 4, 4);
                    constexpr uint32_t lanes = 0x00ff00ff;
                    uint32_t rb = (a & lanes) + (b & lanes) + (c & lanes) + (e & lanes);
                    uint32_t ga =
                        ((a >> 8) & lanes) + ((b >> 8) & lanes) + ((c >> 8) & lanes) + ((e >> 8) & lanes);
                    uint32_t pixel =
                        (((rb + 0x00020002) >> 2) & lanes) | ((((ga + 0x00020002) >> 2) & lanes) << 8);
                    std::memcpy(half->pixels.data() + (size_t(y) * half->width + x) * 4, &pixel, 4);
                }
            }
            source->half = std::move(half);
        }
        source = source->half;
    }
    return source;
}
BilevelImage decode_mmr(Renderer& r, View data, unsigned width, unsigned height) {
    Value params;
    params.kind = Kind::dict;
    Value k;
    k.kind = Kind::number;
    k.number = -1;
    params.dict.emplace_back("K", k);
    auto tiff = fax_tiff(data, width, height, params);
    auto decoded = wic_decode(r, tiff);
    BilevelImage out{width, height, {}};
    out.pixels.resize(size_t(width) * height);
    for (size_t i = 0; i < out.pixels.size(); ++i)
        out.pixels[i] = decoded.pixels[i * 4] < 128 ? 1 : 0;
    return out;
}
std::shared_ptr<Image> load_image(Renderer& r, Document& d, const Value& input, const Value& resources,
                                  int depth, double target_width, double target_height) {
    ProfileTimer load_timer(r.profiling ? &r.profile.image_load_ms : nullptr);
    require(depth < 16, "Image mask recursion limit exceeded");
    const auto& image = resolve(d, input);
    unsigned width = unsigned(std::max(0, integer(entry(d, image, "Width")))),
             height = unsigned(std::max(0, integer(entry(d, image, "Height"))));
    require(width && height && uint64_t(width) * height <= max_stream / 4,
            "Invalid or oversized image dimensions");
    unsigned reduction = 0;
    if (!r.full_resolution_images && target_width > 0 && target_height > 0) {
        target_width = std::max(1.0, target_width);
        target_height = std::max(1.0, target_height);
        while (reduction < 16 && std::ceil(double(width) / (2u << reduction)) >= target_width &&
               std::ceil(double(height) / (2u << reduction)) >= target_height)
            ++reduction;
    }
    auto cached = r.images.find(&image);
    if (image.object && cached != r.images.end() && cached->second.image->reduction <= reduction) {
        cached->second.used = ++r.image_clock;
        if (r.profiling)
            ++r.profile.image_hits;
        return cached->second.image;
    }
    if (r.profiling)
        ++r.profile.image_misses;
    bool stencil = boolean(get(image, "ImageMask"));
    int bits = integer(get(image, "BitsPerComponent"), stencil ? 1 : 8);
    require(bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16, "Unsupported image bit depth");
    auto space = color_space(d, get(image, "ColorSpace"), resources);
    if (stencil)
        space = {};
    ProfileTimer stream_timer(r.profiling ? &r.profile.image_stream_ms : nullptr);
    auto decoded = decode_stream(d, image, true);
    stream_timer.stop();
    size_t profile_index = r.profile.images.size();
    if (r.profiling)
        r.profile.images.push_back({image.object, width, height, decoded.bytes.size(), decoded.image_filter});
    auto out = std::make_shared<Image>();
    out->stencil = stencil;
    auto& decode = entry(d, image, "Decode");
    auto& mask = entry(d, image, "Mask");
    ProfileTimer convert_timer(r.profiling ? &r.profile.image_convert_ms : nullptr);
    if (!decoded.image_filter.empty()) {
        convert_timer.stop();
        ProfileTimer codec_timer(r.profiling ? &r.profile.image_codec_ms : nullptr);
        bool fax = decoded.image_filter == "CCITTFaxDecode" || decoded.image_filter == "CCF";
        bool cmyk_jpeg = (decoded.image_filter == "DCTDecode" || decoded.image_filter == "DCT") &&
                         space.components == 4 && !stencil;
        Bytes tiff;
        bool jpx = decoded.image_filter == "JPXDecode";
        if (jpx) {
            bool indexed = space.name == "Indexed" || space.name == "I";
            auto raw = decode_jpx(d, decoded.bytes,
                                  {r.image_workers, r.profiling ? &r.profile.jpx : nullptr, reduction, indexed});
            require(raw.source_width == width && raw.source_height == height,
                    "Image codec dimensions disagree with the PDF dictionary");
            out->reduction = raw.reduction;
            out->width = raw.width;
            out->height = raw.height;
            int alpha = raw.alpha;
            int embedded_alpha = integer(get(image, "SMaskInData"));
            if (embedded_alpha && alpha < 0 && raw.components > 1)
                alpha = int(raw.components) - 1;
            unsigned channels = raw.components - (alpha >= 0 ? 1 : 0);
            if (get(image, "ColorSpace").kind == Kind::null) {
                require(channels == 1 || channels == 3 || channels == 4,
                        "Unsupported JPEG 2000 color channels");
                space.name = channels == 1 ? "DeviceGray" : channels == 3 ? "DeviceRGB" : "DeviceCMYK";
                space.components = int(channels);
            }
            require(channels == unsigned(space.components), "JPEG 2000 color component mismatch");
            out->pixels.resize(size_t(raw.width) * raw.height * 4);
            const ColorSpace* direct = &space;
            while (direct->name == "ICCBased" && direct->alternate)
                direct = direct->alternate.get();
            if (alpha < 0 && ((direct->name == "DeviceRGB" && channels == 3) ||
                              (direct->name == "DeviceGray" && channels == 1))) {
                for (unsigned y = 0; y < raw.height; ++y) {
                    if ((y & 127) == 0)
                        check_cancel(d);
                    const uint8_t* source = raw.samples.data() + size_t(y) * raw.width * channels;
                    uint8_t* destination = out->pixels.data() + size_t(y) * raw.width * 4;
                    for (unsigned x = 0; x < raw.width; ++x, source += channels, destination += 4) {
                        destination[0] = source[channels == 3 ? 2 : 0];
                        destination[1] = source[channels == 3 ? 1 : 0];
                        destination[2] = source[0];
                        destination[3] = 255;
                    }
                }
            } else
                for (size_t i = 0, p = 0; i < raw.samples.size(); i += raw.components, p += 4) {
                    std::array<double, 32> values{};
                    unsigned channel = 0;
                    double a = alpha >= 0 ? raw.samples[i + size_t(alpha)] / 255.0 : 1;
                    for (unsigned c = 0; c < raw.components; ++c)
                        if (int(c) != alpha) {
                            double value = indexed ? raw.samples[i + c] : raw.samples[i + c] / 255.0;
                            if ((embedded_alpha == 2 || raw.premultiplied) && a > 0)
                                value /= a;
                            values[channel++] = value;
                        }
                    auto converted = color(d, space, std::span(values).first(channels));
                    out->pixels[p] = uint8_t(unit(converted.b * a) * 255 + 0.5f);
                    out->pixels[p + 1] = uint8_t(unit(converted.g * a) * 255 + 0.5f);
                    out->pixels[p + 2] = uint8_t(unit(converted.r * a) * 255 + 0.5f);
                    out->pixels[p + 3] = uint8_t(a * 255 + 0.5);
                }
        } else if (decoded.image_filter == "JBIG2Decode") {
            Bytes globals;
            auto& shared = entry(d, decoded.params, "JBIG2Globals");
            if (shared.has_stream)
                globals = decode_stream(d, shared).bytes;
            auto raw = decode_jbig2(r, d, decoded.bytes, globals, width, height);
            out->width = raw.width;
            out->height = raw.height;
            out->pixels.resize(size_t(raw.width) * raw.height * 4);
            for (size_t i = 0; i < raw.pixels.size(); ++i) {
                uint8_t value = raw.pixels[i] ? 0 : 255;
                out->pixels[i * 4] = out->pixels[i * 4 + 1] = out->pixels[i * 4 + 2] = value;
                out->pixels[i * 4 + 3] = 255;
            }
        } else if (fax && integer(get(decoded.params, "K")) < 0 &&
                   boolean(get(decoded.params, "EncodedByteAlign"))) {
            auto raw = decode_mmr_aligned(d, decoded.bytes, width, height);
            out->width = width;
            out->height = height;
            out->pixels.resize(size_t(width) * height * 4);
            for (size_t i = 0; i < raw.pixels.size(); ++i) {
                uint8_t value = raw.pixels[i] ? 0 : 255;
                out->pixels[i * 4] = out->pixels[i * 4 + 1] = out->pixels[i * 4 + 2] = value;
                out->pixels[i * 4 + 3] = 255;
            }
        } else if (fax) {
            tiff = fax_tiff(decoded.bytes, width, height, decoded.params);
            *out = wic_decode(r, tiff);
        } else
            *out = wic_decode(r, decoded.bytes, reduction, width, height, cmyk_jpeg);
        codec_timer.stop();
        ProfileTimer postprocess_timer(r.profiling ? &r.profile.image_convert_ms : nullptr);
        require(out->reduction || (out->width == width && out->height == height),
                "Image codec dimensions disagree with the PDF dictionary");
        out->stencil = stencil;
        bool invert = fax && boolean(get(decoded.params, "BlackIs1"));
        bool gray_jpeg = (decoded.image_filter == "DCTDecode" || decoded.image_filter == "DCT") &&
                         space.components == 1 && !stencil;
        std::array<uint32_t, 256> palette{};
        if (gray_jpeg) {
            // WIC expands a one-component JPEG to equal RGB channels. These
            // bytes are still PDF samples: apply Decode and the PDF color space
            // before drawing. A Separation sample is a tint, not display gray.
            double lo = num(at(decode, 0));
            double hi = num(at(decode, 1), space.name == "Indexed" || space.name == "I" ? 255 : 1);
            for (size_t i = 0; i < palette.size(); ++i) {
                double sample = lo + (hi - lo) * i / 255.0;
                auto c = color(d, space, std::span(&sample, 1));
                if (mask.kind == Kind::array && i >= num(at(mask, 0)) && i <= num(at(mask, 1)))
                    c.a = 0;
                palette[i] = uint32_t(unit(c.b * c.a) * 255 + 0.5f) |
                             (uint32_t(unit(c.g * c.a) * 255 + 0.5f) << 8) |
                             (uint32_t(unit(c.r * c.a) * 255 + 0.5f) << 16) |
                             (uint32_t(unit(c.a) * 255 + 0.5f) << 24);
            }
        }
        std::array<std::array<double, 256>, 4> cmyk_samples{};
        if (cmyk_jpeg)
            for (unsigned channel = 0; channel < 4; ++channel) {
                double low = num(at(decode, 2 * channel)), high = num(at(decode, 2 * channel + 1), 1);
                for (unsigned sample = 0; sample < 256; ++sample)
                    cmyk_samples[channel][sample] = low + (high - low) * sample / 255.0;
            }
        bool transform_samples = cmyk_jpeg || gray_jpeg || invert || stencil ||
                                 (!jpx && (decode.array.size() == 2 || decode.array.size() == 6));
        for (size_t p = 0; transform_samples && p < out->pixels.size(); p += 4) {
            if (cmyk_jpeg) {
                std::array<double, 4> samples{};
                bool transparent = mask.kind == Kind::array;
                for (unsigned channel = 0; channel < 4; ++channel) {
                    // WIC normalizes four-channel JPEG samples to CMYK.
                    // PDF Decode applies to the original, inverted samples.
                    auto sample = uint8_t(255 - out->pixels[p + channel]);
                    samples[channel] = cmyk_samples[channel][sample];
                    transparent = transparent && sample >= num(at(mask, 2 * channel)) &&
                                  sample <= num(at(mask, 2 * channel + 1));
                }
                auto converted = color(d, space, samples);
                out->pixels[p] = transparent ? 0 : uint8_t(unit(converted.b) * 255 + 0.5f);
                out->pixels[p + 1] = transparent ? 0 : uint8_t(unit(converted.g) * 255 + 0.5f);
                out->pixels[p + 2] = transparent ? 0 : uint8_t(unit(converted.r) * 255 + 0.5f);
                out->pixels[p + 3] = transparent ? 0 : 255;
                continue;
            }
            if (gray_jpeg) {
                uint32_t pixel = palette[out->pixels[p]];
                std::memcpy(out->pixels.data() + p, &pixel, 4);
                continue;
            }
            if (invert)
                for (int c = 0; c < 3; ++c)
                    out->pixels[p + c] = uint8_t(255 - out->pixels[p + c]);
            if (!jpx && decode.array.size() == 2) {
                double lo = num(at(decode, 0)), hi = num(at(decode, 1), 1);
                for (int c = 0; c < 3; ++c)
                    out->pixels[p + c] =
                        uint8_t(unit(lo + (hi - lo) * out->pixels[p + c] / 255.0) * 255 + 0.5f);
            } else if (!jpx && decode.array.size() == 6)
                for (int c = 0; c < 3; ++c) {
                    double lo = num(at(decode, 2 * c)), hi = num(at(decode, 2 * c + 1), 1);
                    out->pixels[p + 2 - c] =
                        uint8_t(unit(lo + (hi - lo) * out->pixels[p + 2 - c] / 255.0) * 255 + 0.5f);
                }
            if (stencil) {
                uint8_t a = uint8_t(255 - out->pixels[p]);
                out->pixels[p] = out->pixels[p + 1] = out->pixels[p + 2] = 0;
                out->pixels[p + 3] = a;
            }
        }
    } else {
        out->width = width;
        out->height = height;
        unsigned n = stencil ? 1 : unsigned(space.components);
        size_t row = (size_t(width) * n * bits + 7) / 8;
        require(height <= decoded.bytes.size() / row, "Truncated image samples");
        std::array<double, 32> low{}, high{};
        double maximum = double((1u << bits) - 1);
        for (unsigned i = 0; i < n; ++i) {
            low[i] = num(at(decode, 2 * i));
            double default_high = (space.name == "Indexed" || space.name == "I") ? maximum : 1;
            high[i] = num(at(decode, 2 * i + 1), default_high);
            if (space.name == "Lab" && decode.kind == Kind::null) {
                low[i] = i == 0 ? 0 : -100;
                high[i] = i == 0 ? 100 : 100;
                auto& range = entry(d, at(space.definition, 1), "Range");
                if (i) {
                    low[i] = num(at(range, 2 * (i - 1)), -100);
                    high[i] = num(at(range, 2 * (i - 1) + 1), 100);
                }
            }
        }
        const ColorSpace* direct = &space;
        while (direct->name == "ICCBased" && direct->alternate)
            direct = direct->alternate.get();
        bool rgb = n == 3 && bits == 8 && direct->name == "DeviceRGB" && mask.kind != Kind::array;
        bool half_rgb = rgb && reduction && width > target_width * 2 && height > target_height * 2 &&
                        !mask.has_stream && !entry(d, image, "SMask").has_stream;
        if (half_rgb) {
            out->width = (width + 1) / 2;
            out->height = (height + 1) / 2;
            out->reduction = 1;
        }
        out->pixels.resize(size_t(out->width) * out->height * 4);
        if (rgb) {
            std::array<std::array<uint8_t, 256>, 3> channels{};
            for (unsigned i = 0; i < 3; ++i)
                for (unsigned sample = 0; sample < 256; ++sample)
                    channels[i][sample] = uint8_t(unit(low[i] + (high[i] - low[i]) * sample / 255.0) *
                                                     255 +
                                                 0.5f);
            for (unsigned y = 0; y < out->height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                uint8_t* destination = out->pixels.data() + size_t(y) * out->width * 4;
                if (half_rgb) {
                    // Fuse conversion with the first box-filter level. This
                    // keeps its rounding without a full-size BGRA allocation.
                    const uint8_t* row0 = decoded.bytes.data() + size_t(y * 2) * row;
                    const uint8_t* row1 =
                        decoded.bytes.data() + size_t(std::min(y * 2 + 1, height - 1)) * row;
                    for (unsigned x = 0; x < out->width; ++x, destination += 4) {
                        size_t x0 = size_t(x * 2) * 3, x1 = size_t(std::min(x * 2 + 1, width - 1)) * 3;
                        for (unsigned c = 0; c < 3; ++c)
                            destination[2 - c] = uint8_t((unsigned(channels[c][row0[x0 + c]]) +
                                                          channels[c][row0[x1 + c]] +
                                                          channels[c][row1[x0 + c]] +
                                                          channels[c][row1[x1 + c]] + 2) /
                                                         4);
                        destination[3] = 255;
                    }
                } else {
                    const uint8_t* source = decoded.bytes.data() + size_t(y) * row;
                    for (unsigned x = 0; x < width; ++x, source += 3, destination += 4) {
                        destination[0] = channels[2][source[2]];
                        destination[1] = channels[1][source[1]];
                        destination[2] = channels[0][source[0]];
                        destination[3] = 255;
                    }
                }
            }
        } else if (n == 1 && bits <= 8) {
            std::array<uint32_t, 256> palette{};
            for (size_t i = 0; i < (size_t(1) << bits); ++i) {
                double value = low[0] + (high[0] - low[0]) * i / maximum;
                Color c = stencil ? Color{0, 0, 0, 1 - unit(value)} : color(d, space, std::span(&value, 1));
                if (mask.kind == Kind::array && i >= num(at(mask, 0)) && i <= num(at(mask, 1)))
                    c.a = 0;
                palette[i] =
                    uint32_t(unit(c.b * c.a) * 255 + 0.5f) | (uint32_t(unit(c.g * c.a) * 255 + 0.5f) << 8) |
                    (uint32_t(unit(c.r * c.a) * 255 + 0.5f) << 16) | (uint32_t(unit(c.a) * 255 + 0.5f) << 24);
            }
            for (unsigned y = 0; y < height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                const uint8_t* source = decoded.bytes.data() + size_t(y) * row;
                uint8_t* destination = out->pixels.data() + size_t(y) * width * 4;
                for (unsigned x = 0; x < width; ++x) {
                    unsigned sample = bits == 8
                                          ? source[x]
                                          : (source[size_t(x) * bits / 8] >> (8 - bits - (x * bits % 8))) &
                                                ((1u << bits) - 1);
                    std::memcpy(destination + size_t(x) * 4, &palette[sample], 4);
                }
            }
        } else
            for (unsigned y = 0; y < height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                size_t bit = size_t(y) * row * 8;
                for (unsigned x = 0; x < width; ++x) {
                    std::array<double, 32> components{};
                    bool transparent = mask.kind == Kind::array;
                    for (unsigned i = 0; i < n; ++i) {
                        unsigned sample = 0;
                        if (bits == 8) {
                            sample = decoded.bytes[bit / 8];
                            bit += 8;
                        } else if (bits == 16) {
                            sample = unsigned(decoded.bytes[bit / 8]) * 256 + decoded.bytes[bit / 8 + 1];
                            bit += 16;
                        } else {
                            sample = (decoded.bytes[bit / 8] >> (8 - bits - (bit % 8))) & ((1u << bits) - 1);
                            bit += bits;
                        }
                        components[i] = low[i] + (high[i] - low[i]) * sample / maximum;
                        if (transparent &&
                            (sample < num(at(mask, i * 2)) || sample > num(at(mask, i * 2 + 1))))
                            transparent = false;
                    }
                    Color c = stencil ? Color{0, 0, 0, 1 - unit(components[0])}
                                      : color(d, space, std::span(components).first(n));
                    if (transparent)
                        c.a = 0;
                    size_t pos = (size_t(y) * width + x) * 4;
                    out->pixels[pos] = uint8_t(unit(c.b * c.a) * 255 + 0.5f);
                    out->pixels[pos + 1] = uint8_t(unit(c.g * c.a) * 255 + 0.5f);
                    out->pixels[pos + 2] = uint8_t(unit(c.r * c.a) * 255 + 0.5f);
                    out->pixels[pos + 3] = uint8_t(unit(c.a) * 255 + 0.5f);
                }
            }
    }
    convert_timer.stop();
    width = out->width;
    height = out->height;
    auto& soft = entry(d, image, "SMask");
    const Value* alpha = soft.has_stream ? &soft : mask.has_stream ? &mask : nullptr;
    if (alpha) {
        auto mask_image = load_image(r, d, *alpha, resources, depth + 1, width, height);
        ProfileTimer mask_timer(r.profiling ? &r.profile.image_mask_ms : nullptr);
        if (out->reduction)
            mask_image = image_level(d, mask_image, width * 0.5, height * 0.5);
        auto& matte = entry(d, *alpha, "Matte");
        Color matte_color;
        if (matte.kind == Kind::array) {
            std::vector<double> m;
            for (auto& v : matte.array)
                m.push_back(num(v));
            matte_color = color(d, space, m);
        }
        if (mask_image->width == width && mask_image->height == height && matte.kind != Kind::array) {
            for (unsigned y = 0; y < height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                size_t end = size_t(y + 1) * width * 4;
                for (size_t p = size_t(y) * width * 4; p < end; p += 4) {
                    unsigned a = mask_image->pixels[p + (mask_image->stencil ? 3 : 0)];
                    if (a == 255)
                        continue;
                    for (unsigned c = 0; c < 4; ++c)
                        out->pixels[p + c] = uint8_t(unsigned(out->pixels[p + c]) * a / 255);
                }
            }
        } else
            for (unsigned y = 0; y < height; ++y) {
                if ((y & 127) == 0)
                    check_cancel(d);
                for (unsigned x = 0; x < width; ++x) {
                    size_t p = (size_t(y) * width + x) * 4;
                    unsigned mx = unsigned(uint64_t(x) * mask_image->width / width),
                             my = unsigned(uint64_t(y) * mask_image->height / height);
                    size_t q = (size_t(my) * mask_image->width + mx) * 4;
                    unsigned a = mask_image->stencil ? mask_image->pixels[q + 3] : mask_image->pixels[q];
                    for (int c = 0; c < 3; ++c) {
                        if (matte.kind == Kind::array) {
                            float m = c == 0 ? matte_color.b : c == 1 ? matte_color.g : matte_color.r;
                            out->pixels[p + c] = uint8_t(
                                std::clamp(double(out->pixels[p + c]) - m * (255 - a), 0.0, double(a)));
                        } else
                            out->pixels[p + c] = uint8_t(unsigned(out->pixels[p + c]) * a / 255);
                    }
                    out->pixels[p + 3] = uint8_t(unsigned(out->pixels[p + 3]) * a / 255);
                }
            }
    }
    if (!r.full_resolution_images && reduction && decoded.image_filter.empty()) {
        // Keep the same box-filtered pixels used for drawing, after all masks
        // are applied. Large raw images can then fit in the image cache.
        ProfileTimer resample_timer(r.profiling ? &r.profile.image_resample_ms : nullptr);
        out = image_level(d, std::move(out), target_width, target_height);
    }
    if (r.profiling) {
        auto& profile = r.profile.images[profile_index];
        profile.decoded_width = out->width;
        profile.decoded_height = out->height;
        profile.reduction = out->reduction;
    }
    if (image.object && out->pixels.size() <= 64 * 1024 * 1024) {
        size_t cost = out->pixels.size() + out->pixels.size() / 3 + 4096;
        if (cost > r.image_cache_limit)
            return out;
        if (auto previous = r.images.find(&image); previous != r.images.end()) {
            r.image_bytes -= previous->second.cost;
            r.images.erase(previous);
        }
        while (!r.images.empty() && r.image_bytes + cost > r.image_cache_limit) {
            auto oldest =
                std::min_element(r.images.begin(), r.images.end(),
                                 [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            r.image_bytes -= oldest->second.cost;
            r.images.erase(oldest);
        }
        r.image_bytes += cost;
        r.images[&image] = {out, cost, ++r.image_clock};
    }
    return out;
}
void save_png(Renderer& r, const RenderedPage& page, const std::filesystem::path& path) {
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    try {
        hr(r.wic->CreateStream(&stream), "Create PNG stream");
        hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG output");
        hr(r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        hr(encoder->Initialize(stream, WICBitmapEncoderNoCache), "Initialize PNG encoder");
        hr(encoder->CreateNewFrame(&frame, &props), "Create PNG frame");
        hr(frame->Initialize(props), "Initialize PNG frame");
        hr(frame->SetSize(page.width, page.height), "Set PNG dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr(frame->SetPixelFormat(&format), "Set PNG format");
        require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "Unexpected PNG encoder format");
        hr(frame->WritePixels(page.height, page.width * 4, UINT(page.pixels.size()),
                              const_cast<BYTE*>(page.pixels.data())),
           "Write PNG pixels");
        hr(frame->Commit(), "Commit PNG frame");
        hr(encoder->Commit(), "Commit PNG file");
    } catch (...) {
        release(props);
        release(frame);
        release(encoder);
        release(stream);
        throw;
    }
    release(props);
    release(frame);
    release(encoder);
    release(stream);
}
} // namespace pdf
