#include "image_codecs.h"
#include "render.h"
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && std::wstring_view(argv[1]) == L"--jbig2") {
        pdf::Document document;
        pdf::Renderer renderer;
        pdf::Mapping input;
        try {
            pdf::map_file(input, argv[2]);
            auto decoded = pdf::decode_jbig2(renderer, document, input.bytes, {},
                                             unsigned(std::stoul(argv[4])), unsigned(std::stoul(argv[5])));
            std::ofstream output(argv[3], std::ios::binary);
            output.write(reinterpret_cast<const char*>(decoded.pixels.data()),
                         std::streamsize(decoded.pixels.size()));
            pdf::require(bool(output), "Cannot write JBIG2 samples");
            pdf::unmap_file(input);
            return 0;
        } catch (const std::exception& error) {
            pdf::unmap_file(input);
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--pdf-images") {
        pdf::Document doc;
        try {
            pdf::open(doc, argv[2]);
            std::filesystem::create_directories(argv[3]);
            for (unsigned i = 1; i < doc.xref.size(); ++i) {
                if (doc.xref[i].type != 1 && doc.xref[i].type != 2)
                    continue;
                const auto& obj = pdf::resolve(doc, pdf::reference(i, doc.xref[i].generation));
                if (!pdf::named(pdf::get(obj, "Subtype"), "Image"))
                    continue;
                auto decoded = pdf::decode_stream(doc, obj, true);
                auto name = std::filesystem::path(argv[3]) / (std::to_wstring(i) + L".bin");
                std::ofstream out(name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(decoded.bytes.data()),
                          std::streamsize(decoded.bytes.size()));
                std::cout << i << " " << pdf::integer(pdf::get(obj, "Width")) << " "
                          << pdf::integer(pdf::get(obj, "Height")) << " " << decoded.image_filter
                          << " globals=" << pdf::get(decoded.params, "JBIG2Globals").object << "\n";
                const auto& globals = pdf::entry(doc, decoded.params, "JBIG2Globals");
                if (globals.has_stream) {
                    auto bytes = pdf::decode_stream(doc, globals).bytes;
                    std::ofstream g(std::filesystem::path(argv[3]) / (std::to_wstring(i) + L"-globals.bin"),
                                    std::ios::binary);
                    g.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                }
            }
            pdf::close(doc);
            return 0;
        } catch (const std::exception& error) {
            pdf::close(doc);
            std::cerr << error.what() << "\n";
            return 1;
        }
    }
    if (argc < 3 || argc > 5)
        return 2;
    pdf::Document document;
    pdf::Mapping input;
    try {
        pdf::map_file(input, argv[1]);
        pdf::JpxOptions options;
        if (argc >= 4) {
            int reduction = std::stoi(argv[3]);
            pdf::require(reduction >= 0 && reduction <= 16, "Invalid JPEG 2000 reduction");
            options.reduction = unsigned(reduction);
        }
        if (argc >= 5) {
            int workers = std::stoi(argv[4]);
            pdf::require(workers >= 0 && workers <= 16, "Invalid JPEG 2000 worker count");
            options.workers = unsigned(workers);
        }
        auto decoded = pdf::decode_jpx(document, input.bytes, options);
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char*>(decoded.samples.data()),
                     std::streamsize(decoded.samples.size()));
        if (!output)
            pdf::fail("Cannot write decoded samples");
        std::cout << decoded.width << " " << decoded.height << " " << decoded.components << "\n";
        pdf::unmap_file(input);
        return 0;
    } catch (const std::exception& error) {
        pdf::unmap_file(input);
        std::cerr << error.what() << "\n";
        return 1;
    }
}
