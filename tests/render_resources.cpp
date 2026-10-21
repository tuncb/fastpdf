#include "render.h"
#include "parallel.h"
#include <iostream>

static void test(bool condition, const char* message) {
    pdf::require(condition, message);
}
static pdf::Value object(std::string_view text) {
    pdf::Parser parser{pdf::view(text)};
    return pdf::parse(parser);
}
static void shared_type1_programs(pdf::Renderer& renderer) {
    pdf::clear_resources(renderer);
    pdf::Document doc;
    std::string header = "%!PS-AdobeFont-1.0\n/Encoding 256 array dup 65 /B put def\ncurrentfile eexec\n";
    std::string plain = "abcd/lenIV -1 def /Subrs 0 array def /CharStrings 1 dict /B 5 RD ";
    plain.append("\x8b\xf8\xec\x0d\x0e", 5); // 0 600 hsbw endchar
    plain += " ND end closefile";
    pdf::Bytes bytes(header.begin(), header.end());
    uint16_t seed = 55665;
    for (uint8_t value : plain) {
        uint8_t encrypted = value ^ uint8_t(seed >> 8);
        bytes.push_back(encrypted);
        seed = uint16_t((unsigned(encrypted) + seed) * 52845 + 22719);
    }
    auto program = std::make_unique<pdf::Value>(object("<< >>"));
    program->has_stream = true;
    program->stream = bytes;
    doc.objects[77] = std::move(program);
    doc.xref.resize(78);
    doc.xref[77].type = 1;
    auto native = object("<< /Subtype /Type1 /BaseFont /Test /FirstChar 65 /Widths [600] "
                         "/FontDescriptor << /FontFile 77 0 R >> >>");
    auto remapped = object("<< /Subtype /Type1 /BaseFont /Test /FirstChar 66 /Widths [900] "
                           "/Encoding << /Differences [66 /B] >> "
                           "/FontDescriptor << /FontFile 77 0 R >> >>");
    // Load the explicit encoding first. It must not suppress the native encoding
    // when another dictionary later uses the same font program.
    auto& a = pdf::load_font(renderer, doc, remapped);
    auto& b = pdf::load_font(renderer, doc, native);
    test(a.type1 && b.type1 && a.type1_program == b.type1_program && renderer.type1_programs.size() == 1,
         "Font dictionaries share the decoded Type 1 program");
    auto ga = pdf::decode_text(a, "B"), gb = pdf::decode_text(b, "A");
    test(ga.size() == 1 && gb.size() == 1 && ga[0].text == L"B" && gb[0].text == L"B" &&
             ga[0].advance == 900 && gb[0].advance == 600,
         "Shared font programs preserve separate encodings and widths");
    auto inherited = pdf::decode_text(a, "A");
    test(inherited.size() == 1 && inherited[0].text == L"B" && inherited[0].name == "B",
         "Differences without BaseEncoding retain the native Type 1 mapping");
    pdf::clear_resources(renderer);
    test(renderer.type1_programs.empty(), "Document changes clear shared Type 1 programs");
}
static void raw_images(pdf::Renderer& renderer, pdf::Document& doc) {
    pdf::clear_resources(renderer);
    renderer.image_cache_limit = 128 * 1024 * 1024;
    renderer.full_resolution_images = false;
    pdf::Bytes samples(257 * 17 * 3);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = uint8_t((i * 73 + i / 11) % 256);
    for (bool icc : {false, true})
        for (bool remap : {false, true}) {
            auto source = object("<< /Width 257 /Height 17 /BitsPerComponent 8 /ColorSpace /DeviceRGB >>");
            if (icc)
                for (auto& [key, value] : source.dict)
                    if (key == "ColorSpace")
                        value = object("[/ICCBased << /N 3 /Alternate /DeviceRGB >>]");
            if (remap)
                source.dict.emplace_back("Decode", object("[1 0 -0.2 0.7 0.25 1.25]"));
            source.has_stream = true;
            source.stream = samples;
            source.object = 31;
            auto full = pdf::load_image(renderer, doc, source, {});
            for (size_t i = 0; i < samples.size() / 3; ++i) {
                std::array<double, 3> values{samples[i * 3] / 255.0, samples[i * 3 + 1] / 255.0,
                                             samples[i * 3 + 2] / 255.0};
                if (remap)
                    values = {1 - values[0], -0.2 + 0.9 * values[1], 0.25 + values[2]};
                pdf::ColorSpace rgb;
                rgb.name = "DeviceRGB";
                rgb.components = 3;
                auto color = pdf::color(doc, rgb, values);
                test(full->pixels[i * 4] == uint8_t(color.b * 255 + 0.5f) &&
                         full->pixels[i * 4 + 1] == uint8_t(color.g * 255 + 0.5f) &&
                         full->pixels[i * 4 + 2] == uint8_t(color.r * 255 + 0.5f) &&
                         full->pixels[i * 4 + 3] == 255,
                     "Raw RGB conversion matches scalar color conversion");
            }
            auto expected = pdf::image_level(doc, full, 40, 3);
            pdf::clear_resources(renderer);
            auto reduced = pdf::load_image(renderer, doc, source, {}, 0, 40, 3);
            test(reduced->width == expected->width && reduced->height == expected->height &&
                     reduced->pixels == expected->pixels,
                 "Reduced raw image preserves box-filtered pixels, including odd edges");
            test(pdf::load_image(renderer, doc, source, {}, 0, 40, 3) == reduced,
                 "Reuse reduced raw image");
            auto upgraded = pdf::load_image(renderer, doc, source, {});
            test(upgraded->reduction == 0 && upgraded->pixels == full->pixels,
                 "Raw image zoom upgrade restores all source pixels");
            pdf::clear_resources(renderer);
        }
    pdf::Bytes alpha(257 * 17);
    for (size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = uint8_t(i * 43);
    auto mask = object("<< /Width 257 /Height 17 /BitsPerComponent 8 /ColorSpace /DeviceGray >>");
    mask.has_stream = true;
    mask.stream = alpha;
    auto masked = object("<< /Width 257 /Height 17 /BitsPerComponent 8 /ColorSpace /DeviceRGB >>");
    masked.dict.emplace_back("SMask", std::move(mask));
    masked.has_stream = true;
    masked.stream = samples;
    masked.object = 33;
    auto masked_full = pdf::load_image(renderer, doc, masked, {});
    auto masked_expected = pdf::image_level(doc, masked_full, 40, 3);
    pdf::clear_resources(renderer);
    auto masked_reduced = pdf::load_image(renderer, doc, masked, {}, 0, 40, 3);
    test(masked_reduced->pixels == masked_expected->pixels,
         "Raw image masks are applied before display reduction");
    pdf::clear_resources(renderer);
    // A full BGRA bitmap exceeds the 64 MiB entry limit, as in the Mothes figures.
    samples.assign(size_t(4097) * 4096 * 3, 127);
    auto large = object("<< /Width 4097 /Height 4096 /BitsPerComponent 8 /ColorSpace /DeviceRGB >>");
    large.has_stream = true;
    large.stream = samples;
    large.object = 32;
    auto reduced = pdf::load_image(renderer, doc, large, {}, 0, 300, 300);
    test(reduced->width == 513 && reduced->height == 512 && reduced->reduction == 3,
         "Large raw image retains enough display detail");
    test(renderer.images.contains(&large) && renderer.image_bytes <= renderer.image_cache_limit &&
             pdf::load_image(renderer, doc, large, {}, 0, 300, 300) == reduced,
         "Large raw image fits the cache at display size");
    pdf::clear_resources(renderer);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2)
        return 2;
    pdf::Document doc;
    pdf::Renderer renderer;
    pdf::Mapping mapping;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int code = 0;
    try {
        pdf::hr(com, "Initialize test COM");
        pdf::init_renderer(renderer);
        pdf::map_file(mapping, std::filesystem::path(argv[1]) / L"RGB-lossless.jp2");
        pdf::Parser parser{pdf::view(
            "<< /Width 37 /Height 29 /BitsPerComponent 8 /ColorSpace /DeviceRGB /Filter /JPXDecode >>")};
        auto source = pdf::parse(parser);
        source.object = 1;
        source.has_stream = true;
        source.stream = mapping.bytes;
        auto reduced = pdf::load_image(renderer, doc, source, {}, 0, 19, 15);
        test(reduced->width == 19 && reduced->height == 15 && reduced->reduction == 1,
             "Decode at display resolution");
        auto hit = pdf::load_image(renderer, doc, source, {}, 0, 10, 8);
        test(hit == reduced, "Reuse sufficiently detailed cached image");
        auto full = pdf::load_image(renderer, doc, source, {});
        test(full != reduced && full->width == 37 && full->height == 29 && full->reduction == 0,
             "Zoom upgrade replaces reduced image");
        test(renderer.images.size() == 1 && renderer.image_bytes == renderer.images.at(&source).cost,
             "Replacing cached image updates byte accounting");
        test(pdf::load_image(renderer, doc, source, {}, 0, 19, 15) == full,
             "Zoom out reuses full resolution cache");
        auto raw = pdf::decode_jpx(doc, mapping.bytes, {1});
        for (size_t i = 0; i < raw.samples.size() / 3; ++i)
            test(full->pixels[i * 4] == raw.samples[i * 3 + 2] &&
                     full->pixels[i * 4 + 1] == raw.samples[i * 3 + 1] &&
                     full->pixels[i * 4 + 2] == raw.samples[i * 3] && full->pixels[i * 4 + 3] == 255,
                 "Full resolution RGB conversion preserves every sample");
        pdf::clear_resources(renderer);
        test(renderer.images.empty() && renderer.image_bytes == 0, "Document change clears image cache");
        renderer.full_resolution_images = true;
        test(pdf::load_image(renderer, doc, source, {}, 0, 5, 4)->reduction == 0,
             "Full resolution override disables codec reduction");
        size_t image_cost = renderer.images.at(&source).cost;
        pdf::clear_resources(renderer);
        renderer.image_cache_limit = 2 * image_cost;
        auto second = source, third = source;
        second.object = 2;
        third.object = 3;
        pdf::load_image(renderer, doc, source, {});
        auto retained = pdf::load_image(renderer, doc, second, {});
        pdf::load_image(renderer, doc, source, {});
        pdf::load_image(renderer, doc, third, {});
        test(renderer.images.contains(&source) && renderer.images.contains(&third) &&
                 !renderer.images.contains(&second) && renderer.image_bytes <= renderer.image_cache_limit,
             "Cache pressure evicts the least recently used image only");
        test(retained->pixels == full->pixels, "Eviction does not invalidate an in-flight image");

        // Compare the packed box filter with its scalar definition, including
        // every combination of odd/even edges and premultiplied alpha values.
        for (unsigned h = 3; h <= 12; ++h)
            for (unsigned w = 3; w <= 12; ++w) {
                auto image = std::make_shared<pdf::Image>();
                image->width = w;
                image->height = h;
                image->pixels.resize(size_t(w) * h * 4);
                for (size_t i = 0; i < image->pixels.size(); ++i)
                    image->pixels[i] = uint8_t((i * 73 + i / 7) % 256);
                auto half = pdf::image_level(doc, image, (w + 1) / 4.0, (h + 1) / 4.0);
                test(half->width == (w + 1) / 2 && half->height == (h + 1) / 2, "Pyramid dimensions");
                for (unsigned y = 0; y < half->height; ++y)
                    for (unsigned x = 0; x < half->width; ++x)
                        for (unsigned c = 0; c < 4; ++c) {
                            unsigned sum = 0, count = 0;
                            for (unsigned yy = y * 2; yy < std::min(y * 2 + 2, h); ++yy)
                                for (unsigned xx = x * 2; xx < std::min(x * 2 + 2, w); ++xx) {
                                    sum += image->pixels[(size_t(yy) * w + xx) * 4 + c];
                                    ++count;
                                }
                            test(half->pixels[(size_t(y) * half->width + x) * 4 + c] ==
                                     (sum + count / 2) / count,
                                 "Packed filter matches scalar box filter");
                        }
            }

        raw_images(renderer, doc);
        shared_type1_programs(renderer);
        std::atomic<unsigned> completed = 0, active = 0;
        bool failed = false;
        try {
            pdf::parallel_for(256, 1, 4, [&](size_t index) {
                ++active;
                ++completed;
                --active;
                if (index == 7)
                    pdf::fail("Expected parallel failure");
            });
        } catch (const std::exception& error) {
            failed = std::string_view(error.what()) == "Expected parallel failure";
        }
        test(failed && active == 0 && completed <= 256, "Parallel failures join workers and propagate");
        completed = 0;
        pdf::parallel_for(256, 4, 4, [&](size_t) { ++completed; });
        test(completed == 256, "Pool remains usable after failure");
        std::atomic<bool> canceled = true;
        doc.cancel = &canceled;
        bool interrupted = false;
        try {
            pdf::decode_jpx(doc, mapping.bytes);
        } catch (const std::exception&) {
            interrupted = true;
        }
        doc.cancel = nullptr;
        test(interrupted, "JPEG 2000 honors cancellation");
        std::cout << "Resolution upgrades, image cache, box filter, worker failure and cancellation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        code = 1;
    }
    pdf::close_renderer(renderer);
    pdf::unmap_file(mapping);
    if (SUCCEEDED(com))
        CoUninitialize();
    return code;
}
