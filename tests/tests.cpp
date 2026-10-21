#include "render.h"
#include "image_codecs.h"
#include <bcrypt.h>
#include <iostream>
#include <functional>

static int assertions = 0;
static void test(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}
static void rejects(const std::function<void()>& action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    test(rejected, message);
}
static pdf::Value object(std::string_view source) {
    pdf::Parser p{pdf::view(source)};
    return pdf::parse(p);
}
static void png_predictors() {
    constexpr unsigned filters[] = {2, 0, 1, 2, 3, 4, 2, 0, 4, 1};
    for (unsigned columns : {1u, 2u, 3u, 15u, 16u, 17u, 31u, 32u, 33u, 257u})
        for (unsigned colors : {1u, 3u, 4u}) {
            size_t row = size_t(columns) * colors;
            pdf::Bytes expected(row * std::size(filters)), encoded;
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = uint8_t(i * 61 + i / 7);
            for (size_t y = 0; y < std::size(filters); ++y) {
                encoded.push_back(uint8_t(filters[y]));
                for (size_t x = 0; x < row; ++x) {
                    int left = x >= colors ? expected[y * row + x - colors] : 0;
                    int above = y ? expected[(y - 1) * row + x] : 0;
                    int corner = y && x >= colors ? expected[(y - 1) * row + x - colors] : 0;
                    int predicted = 0;
                    if (filters[y] == 1)
                        predicted = left;
                    else if (filters[y] == 2)
                        predicted = above;
                    else if (filters[y] == 3)
                        predicted = (left + above) / 2;
                    else if (filters[y] == 4) {
                        int p = left + above - corner;
                        std::array<int, 3> values{left, above, corner};
                        std::array<int, 3> distances{std::abs(p - left), std::abs(p - above),
                                                     std::abs(p - corner)};
                        predicted = values[size_t(std::min_element(distances.begin(), distances.end()) -
                                                  distances.begin())];
                    }
                    encoded.push_back(uint8_t(expected[y * row + x] - predicted));
                }
            }
            auto params = object("<< /Predictor 15 /BitsPerComponent 8 /Colors " + std::to_string(colors) +
                                 " /Columns " + std::to_string(columns) + " >>");
            test(pdf::predictor(encoded, params) == expected,
                 "Mixed PNG filters preserve rows across vector and scalar boundaries");
        }
}
static void named_destinations() {
    pdf::Document doc;
    doc.pages.resize(3);
    for (unsigned i = 0; i < 3; ++i)
        doc.pages[i].reference = pdf::reference(10 + i);
    // Name trees may have multiple branches and duplicate names in damaged
    // files. Keep the first match and the legacy dictionary's precedence.
    doc.catalog = object("<< /Dests << /legacy [10 0 R /Fit] >> /Names << /Dests << /Kids ["
                         "<< /Names [(last) [12 0 R /Fit] (shared) [11 0 R /Fit]] >>"
                         "<< /Names [(legacy) [12 0 R /Fit] (shared) [12 0 R /Fit]"
                         "(first) << /D [10 0 R /Fit] >>] >>] >> >> >>");
    test(pdf::destination_page(doc, object("(last)")) == 2, "Indexed named destination");
    test(pdf::destination_page(doc, object("/legacy")) == 0, "Legacy destination takes precedence");
    test(pdf::destination_page(doc, object("(shared)")) == 1, "First duplicate destination wins");
    test(pdf::destination_page(doc, object("(first)")) == 0, "Destination dictionary");
    test(pdf::destination_page(doc, object("(missing)")) == -1, "Missing named destination");
    pdf::close(doc);
    test(doc.named_destinations.empty() && !doc.destinations_loaded, "Close clears destination pointers");
}
int main() {
    using namespace pdf;
    try {
        auto v = object("<< /N#61me (line\\n\\050nested\\051\\053) /A [1 -2.5 .25 true false null /X] /R 12 "
                        "4 R /Hex <4a6f6> >>");
        test(get(v, "Name").text == "line\n(nested)+", "Literal escape decoding");
        test(get(v, "Hex").text == "Jo`", "Odd hex nibble");
        test(get(v, "A").array.size() == 7, "Array parsing");
        test(at(get(v, "A"), 2).number == 0.25, "Fraction parsing");
        test(get(v, "R").object == 12 && get(v, "R").generation == 4, "Reference parsing");
        rejects([] { object("<< /Key"); }, "Reject truncated dictionary");
        rejects([] { object("(unfinished"); }, "Reject truncated string");
        rejects([] { object(std::string(130, '[')); }, "Bound parser nesting");
        test(string(ascii_hex(view("61 62 6>"))) == "ab`", "ASCIIHex decoding");
        test(string(ascii85(view("<~87cURD_*#TDfTZ)+T~>"))) == "Hello, world!", "ASCII85 decoding");
        test(ascii85(view("z~>")).size() == 4, "ASCII85 zero tuple");
        rejects([] { ascii85(view("!z")); }, "Reject invalid ASCII85 tuple");
        const uint8_t rle[] = {2, 'a', 'b', 'c', 254, 'x', 128};
        test(string(run_length(rle)) == "abcxxx", "RunLength decoding");
        // ITU-T T.88 H.2's single-context arithmetic known-answer sequence.
        auto mq_bytes = ascii_hex(view("84c73bfce1a1430402200000410dbb86f4317fff88ff37471adb6adfffac"));
        auto mq_expected = ascii_hex(view("00 02 00 51 00 00 00 c0 03 52 87 2a aa aa aa aa 82 c0 20 00 fc d7 "
                                          "9e f6 bf 7f ed 90 4f 46 a3 bf"));
        MQDecoder mq;
        mq_init(mq, mq_bytes);
        uint8_t context = 0;
        Bytes mq_result(mq_expected.size());
        for (auto& byte : mq_result)
            for (unsigned i = 0; i < 8; ++i)
                byte = uint8_t((byte << 1) | mq_bit(mq, context));
        test(mq_result == mq_expected, "ITU-T T.88 H.2 arithmetic known answer");
        const uint8_t empty[] = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        test(inflate(empty).empty(), "Empty DEFLATE stream");
        Bytes padded_zlib(std::begin(empty), std::end(empty));
        padded_zlib.insert(padded_zlib.end(), 8, 0);
        test(inflate(padded_zlib).empty(), "Zlib end marker with PDF padding");
        const uint8_t stored[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff,
                                  'a',  'b',  'c',  0x02, 0x4d, 0x01, 0x27};
        test(string(inflate(stored)) == "abc", "Stored DEFLATE block");
        // Python zlib output covers repeated bytes and overlapping multibyte runs.
        for (auto [pattern, encoded] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {"A", "789c2b284a4dcbac701c05a360140c7b505c9a06cced009975031b"},
                 {"AB", "789c2b284a4dcbac70741a85a370148ec251380a47e15087c5a569c05a1d00789104fa"},
                 {"ABC", "789c2b284a4dcbac7074721e45a368148da251348a46d1281a45a368140d72545c9a066cbd0300a1630ac1"},
                 {"ABCDEFG", "789cedc5c10d80201000b0d9548449b8e47e466272e3bb0113b49f3eef8cace3bcdadd87244992244992244992a44deb8bc8fa01dd3a48ed"}}) {
            std::string expected = "prefix";
            for (int i = 0; i < 1000; ++i)
                expected += pattern;
            expected += "suffix";
            auto compressed = ascii_hex(view(encoded));
            test(string(inflate(compressed)) == expected, "Overlapping DEFLATE runs match zlib output");
            rejects([&] { inflate(compressed, expected.size() - 1); }, "Repeated DEFLATE output budget");
        }
        rejects([&] { inflate(stored, 2); }, "DEFLATE output budget");
        auto corrupt = Bytes(std::begin(stored), std::end(stored));
        corrupt.back() ^= 1;
        rejects([&] { inflate(corrupt); }, "DEFLATE checksum");
        auto params = object("<< /Predictor 15 /Columns 3 /Colors 1 /BitsPerComponent 8 >>");
        test(predictor({1, 10, 10, 10, 2, 1, 2, 3}, params) == Bytes({10, 20, 30, 11, 22, 33}),
             "PNG Sub and Up predictors");
        png_predictors();
        named_destinations();
        auto tiff = object("<< /Predictor 2 /Columns 3 /Colors 1 /BitsPerComponent 16 >>");
        test(predictor({0, 255, 0, 1, 0, 1}, tiff) == Bytes({0, 255, 1, 0, 1, 1}), "16-bit TIFF prediction");
        test(rc4(view("Plaintext"), view("Key")) == ascii_hex(view("BBF316E8D940AF0AD3")),
             "RC4 known answer");
        test(digest(BCRYPT_MD5_ALGORITHM, view("abc")) == ascii_hex(view("900150983cd24fb0d6963f7d28e17f72")),
             "MD5 known answer");
        auto key = ascii_hex(view("2b7e151628aed2a6abf7158809cf4f3c")),
             iv = ascii_hex(view("000102030405060708090a0b0c0d0e0f")),
             plain = ascii_hex(view("6bc1bee22e409f96e93d7e117393172a")),
             cipher = ascii_hex(view("7649abac8119b246cee98e9b12e9197d"));
        test(aes(plain, key, iv, true) == cipher, "AES-CBC NIST encryption vector");
        test(aes(cipher, key, iv, false) == plain, "AES-CBC NIST decryption vector");
        Matrix m{2, 0.4, 0.3, 3, 100, 200};
        auto p = transform(inverse(m), transform(m, {7, 9}));
        test(std::abs(p.x - 7) < 1e-10 && std::abs(p.y - 9) < 1e-10, "Matrix inverse");
        test(prepare_password("I\xc2\xadX") == "IX" && prepare_password("\xe2\x85\xa8") == "IX",
             "RFC 4013 mapping and normalization");
        test(prepare_password("a\xc2\xa0z") == "a z" && prepare_password("USER") == "USER",
             "SASLprep spaces and case preservation");
        rejects([] { prepare_password("\x07"); }, "SASLprep prohibited control");
        rejects(
            [] {
                prepare_password("\xd8\xa7"
                                 "1");
            },
            "SASLprep bidirectional rule");
        Document d;
        auto fn = object("<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 2 >>");
        double x = 0.5;
        auto y = evaluate_function(d, fn, std::span(&x, 1));
        test(y.size() == 3 && y[0] == 0.75 && y[2] == 0.25, "Exponential PDF function");
        CMap cmap;
        parse_cmap(view("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfchar <01> <00660069> "
                        "endbfchar 1 beginbfrange <20> <22> <0041> endbfrange"),
                   cmap, true);
        test(cmap.unicode[1] == L"fi" && cmap.unicode[0x22] == L"C", "CMap ranges and ligatures");
        test(glyph_unicode("Aacute") == 0xc1 && glyph_unicode("uni03A9") == 0x3a9, "Glyph names");
        CMap japanese;
        test(builtin_cmap("90ms-RKSJ-V", japanese, false) && japanese.vertical && !japanese.cid.empty(),
             "Predefined Japanese CMap inheritance");
        CMap cid_unicode;
        test(builtin_cmap("Adobe-Japan1-UCS2", cid_unicode, true) && !cid_unicode.unicode.empty(),
             "Adobe CID to Unicode mapping");
        test(glyph_unicode("afii10017") == 0x410, "Published Cyrillic glyph mapping");
        ColorSpace cs;
        cs.name = "DeviceCMYK";
        cs.components = 4;
        double values[] = {1, 0, 0, 0};
        auto rgb = color(d, cs, values);
        test(rgb.r < 0.2 && rgb.g > 0.5 && rgb.b > 0.7, "CMYK cyan display conversion");
        double white[] = {0, 0, 0, 0}, black[] = {0, 0, 0, 1};
        auto paper = color(d, cs, white), ink = color(d, cs, black);
        test(paper.r == 1 && paper.g == 1 && paper.b == 1, "CMYK paper stays white");
        test(ink.r < 0.2 && ink.g < 0.2 && ink.b < 0.2, "CMYK black stays dark");
        std::cout << assertions << " engine assertions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED after " << assertions << " assertions: " << e.what() << '\n';
        return 1;
    }
}
