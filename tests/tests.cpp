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
        rejects([&] { inflate(stored, 2); }, "DEFLATE output budget");
        auto corrupt = Bytes(std::begin(stored), std::end(stored));
        corrupt.back() ^= 1;
        rejects([&] { inflate(corrupt); }, "DEFLATE checksum");
        auto params = object("<< /Predictor 15 /Columns 3 /Colors 1 /BitsPerComponent 8 >>");
        test(predictor({1, 10, 10, 10, 2, 1, 2, 3}, params) == Bytes({10, 20, 30, 11, 22, 33}),
             "PNG Sub and Up predictors");
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
        test(rgb.r == 0 && rgb.g == 1 && rgb.b == 1, "CMYK conversion");
        std::cout << assertions << " engine assertions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED after " << assertions << " assertions: " << e.what() << '\n';
        return 1;
    }
}
