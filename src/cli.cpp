#include "render.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <psapi.h>
#include <sstream>

static std::string json(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 32) {
            char buf[8]{};
            snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
            out += buf;
        } else
            out += char(c);
    }
    return out + '\"';
}
static void write_profile(std::ostream& out, const pdf::RenderProfile& p) {
    out << ",\"profile\":{\"content_ms\":" << p.content_ms << ",\"image_load_ms\":" << p.image_load_ms
        << ",\"image_stream_ms\":" << p.image_stream_ms << ",\"image_codec_ms\":" << p.image_codec_ms
        << ",\"image_convert_ms\":" << p.image_convert_ms << ",\"image_mask_ms\":" << p.image_mask_ms
        << ",\"image_resample_ms\":" << p.image_resample_ms << ",\"image_upload_ms\":" << p.image_upload_ms
        << ",\"composite_ms\":" << p.composite_ms << ",\"finish_ms\":" << p.finish_ms
        << ",\"readback_ms\":" << p.readback_ms << ",\"image_hits\":" << p.image_hits
        << ",\"image_misses\":" << p.image_misses << ",\"jpx_packets_ms\":" << p.jpx.packets_ms
        << ",\"jpx_blocks_ms\":" << p.jpx.blocks_ms << ",\"jpx_wavelet_ms\":" << p.jpx.wavelet_ms
        << ",\"jpx_output_ms\":" << p.jpx.output_ms << ",\"images\":[";
    for (size_t i = 0; i < p.images.size(); ++i) {
        const auto& image = p.images[i];
        if (i)
            out << ',';
        out << "{\"object\":" << image.object << ",\"width\":" << image.width
            << ",\"height\":" << image.height << ",\"bytes\":" << image.bytes
            << ",\"filter\":" << json(image.filter) << ",\"decoded_width\":" << image.decoded_width
            << ",\"decoded_height\":" << image.decoded_height << ",\"reduction\":" << image.reduction << '}';
    }
    out << "]}";
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || std::wstring_view(argv[1]) == L"--help") {
        std::cout
            << "FastPDF engine probe\nUsage: pdfprobe document.pdf [options]\n  --page N          "
               "One-based page number (default 1)\n  --render out.png  Render to PNG\n  --scale N      "
               "   Pixels per PDF point (default 1)\n  --rotate N        Additional clockwise rotation "
               "in degrees\n  --text            Include extracted text in JSON output\n  --extract "
               "out.txt Write extracted text as UTF-8\n  --all             Render and validate every "
               "page\n  --benchmark N     Repeat rendering N times\n  --timings         Include per-render "
               "timings\n"
               "  --profile         Include rendering stage timings and image metadata\n"
               "  --image-workers N Decoder workers (0 automatic, 1 serial, maximum 16)\n"
               "  --full-resolution Disable resolution-aware image decoding\n  --password TEXT   Open a "
               "password-protected PDF\n  --strict          Exit with code 3 if compatibility warnings "
               "occur\n  --info            Only load and inspect the document (default)\n";
        return argc < 2 ? 1 : 0;
    }
    pdf::Document doc;
    pdf::Renderer renderer;
    bool com = false;
    int code = 0;
    try {
        int page = 1, rotation = 0, repeats = 1;
        double scale = 1;
        bool text = false, all = false, strict = false, timings = false, benchmark = false;
        std::filesystem::path output, extract;
        std::string password;
        for (int i = 2; i < argc; ++i) {
            std::wstring arg = argv[i];
            auto next = [&]() -> std::wstring {
                pdf::require(i + 1 < argc, "Missing command line argument");
                return argv[++i];
            };
            if (arg == L"--page")
                page = std::stoi(next());
            else if (arg == L"--scale")
                scale = std::stod(next());
            else if (arg == L"--rotate")
                rotation = std::stoi(next());
            else if (arg == L"--render")
                output = next();
            else if (arg == L"--extract")
                extract = next();
            else if (arg == L"--password")
                password = pdf::utf8(next());
            else if (arg == L"--text")
                text = true;
            else if (arg == L"--all")
                all = true;
            else if (arg == L"--strict")
                strict = true;
            else if (arg == L"--timings")
                timings = true;
            else if (arg == L"--full-resolution")
                renderer.full_resolution_images = true;
            else if (arg == L"--image-workers") {
                int workers = std::stoi(next());
                pdf::require(workers >= 0 && workers <= 16, "Image worker count must be between 0 and 16");
                renderer.image_workers = unsigned(workers);
            } else if (arg == L"--profile") {
                renderer.profiling = true;
                timings = true;
            } else if (arg == L"--benchmark") {
                benchmark = true;
                repeats = std::stoi(next());
            } else if (arg != L"--info")
                pdf::fail("Unknown command line option");
        }
        pdf::require(page > 0 && repeats > 0 && repeats <= 1000, "Invalid page or benchmark count");
        pdf::hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
        com = true;
        auto begin = std::chrono::steady_clock::now();
        pdf::open(doc, argv[1], password);
        double open_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        pdf::require(size_t(page) <= doc.pages.size(), "Page number is out of range");
        bool rendering = !output.empty() || all || benchmark || timings;
        bool needed = rendering || text || !extract.empty();
        pdf::RenderedPage rendered;
        double total_ms = 0;
        size_t rendered_count = 0;
        std::ostringstream samples;
        samples << std::fixed << std::setprecision(3);
        if (needed) {
            pdf::init_renderer(renderer);
            int first = all ? 1 : page, last = all ? int(doc.pages.size()) : page;
            for (int round = 0; round < repeats; ++round)
                for (int p = first; p <= last; ++p) {
                    auto result = pdf::render_page(renderer, doc, size_t(p - 1), scale, rotation, !rendering);
                    if (timings) {
                        if (rendered_count)
                            samples << ',';
                        samples << "{\"page\":" << p << ",\"round\":" << round + 1
                                << ",\"ms\":" << result.milliseconds;
                        if (renderer.profiling)
                            write_profile(samples, renderer.profile);
                        samples << '}';
                    }
                    total_ms += result.milliseconds;
                    ++rendered_count;
                    if (p == page)
                        rendered = std::move(result);
                }
        }
        if (!output.empty())
            pdf::save_png(renderer, rendered, output);
        std::string extracted = needed ? pdf::utf8(pdf::page_text(rendered)) : "";
        if (!extract.empty()) {
            std::ofstream file(extract, std::ios::binary);
            pdf::require(bool(file), "Cannot open text output");
            file << extracted;
            pdf::require(bool(file), "Cannot write text output");
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory));
        std::cout << std::fixed << std::setprecision(3) << "{\"pages\":" << doc.pages.size()
                  << ",\"page\":" << page << ",\"objects_loaded\":" << doc.objects.size()
                  << ",\"open_ms\":" << open_ms << ",\"render_ms\":" << total_ms
                  << ",\"renders\":" << rendered_count << ",\"private_bytes\":" << memory.PrivateUsage
                  << ",\"repaired\":" << (doc.repaired ? "true" : "false")
                  << ",\"encrypted\":" << (doc.security.enabled ? "true" : "false");
        if (needed)
            std::cout << ",\"width\":" << rendered.width << ",\"height\":" << rendered.height
                      << ",\"text_spans\":" << rendered.text.size();
        if (text)
            std::cout << ",\"text\":" << json(extracted);
        if (timings)
            std::cout << ",\"samples\":[" << samples.str() << ']';
        std::cout << ",\"warnings\":[";
        for (size_t i = 0; i < doc.warnings.size(); ++i) {
            if (i)
                std::cout << ',';
            std::cout << json(doc.warnings[i]);
        }
        std::cout << "]}\n";
        if (strict && !doc.warnings.empty())
            code = 3;
    } catch (const std::exception& e) {
        std::cerr << "{\"error\":" << json(e.what()) << "}\n";
        code = 2;
    }
    pdf::close_renderer(renderer);
    pdf::close(doc);
    if (com)
        CoUninitialize();
    return code;
}
