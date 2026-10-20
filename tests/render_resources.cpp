#include "render.h"
#include "parallel.h"
#include <iostream>

static void test(bool condition, const char* message) {
    pdf::require(condition, message);
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
