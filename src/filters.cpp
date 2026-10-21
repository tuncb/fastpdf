#include "pdf.h"
#include <emmintrin.h>

namespace pdf {
struct Bits {
    View data;
    size_t pos = 0;
    uint64_t bits = 0;
    unsigned count = 0;
};
static void fill(Bits& b, unsigned n) {
    while (b.count < n && b.pos < b.data.size()) {
        b.bits |= uint64_t(b.data[b.pos++]) << b.count;
        b.count += 8;
    }
}
static uint32_t take(Bits& b, unsigned n) {
    fill(b, n);
    require(b.count >= n, "Truncated DEFLATE bitstream");
    uint32_t v = uint32_t(b.bits & ((uint64_t(1) << n) - 1));
    b.bits >>= n;
    b.count -= n;
    return v;
}
struct Huffman {
    std::array<uint16_t, 32768> symbols{};
    std::array<uint8_t, 32768> lengths{};
    unsigned max_bits = 0;
};
static Huffman huffman(std::span<const uint8_t> lengths) {
    Huffman h;
    unsigned counts[16]{}, next[16]{};
    for (auto n : lengths) {
        require(n <= 15, "Invalid Huffman length");
        if (n) {
            ++counts[n];
            h.max_bits = std::max(h.max_bits, unsigned(n));
        }
    }
    int remaining = 1;
    for (int n = 1; n <= 15; ++n) {
        remaining = remaining * 2 - int(counts[n]);
        require(remaining >= 0, "Oversubscribed Huffman tree");
    }
    unsigned code = 0;
    for (int n = 1; n <= 15; ++n) {
        code = (code + counts[n - 1]) << 1;
        next[n] = code;
    }
    for (unsigned s = 0; s < lengths.size(); ++s) {
        unsigned n = lengths[s];
        if (!n)
            continue;
        unsigned c = next[n]++, r = 0;
        for (unsigned j = 0; j < n; ++j) {
            r = (r << 1) | (c & 1);
            c >>= 1;
        }
        for (unsigned i = r; i < (1u << h.max_bits); i += 1u << n) {
            h.symbols[i] = uint16_t(s);
            h.lengths[i] = uint8_t(n);
        }
    }
    return h;
}
static unsigned symbol(Bits& b, const Huffman& h) {
    fill(b, h.max_bits);
    unsigned index = unsigned(b.bits & ((1u << h.max_bits) - 1));
    unsigned n = h.lengths[index];
    require(n && n <= b.count, "Invalid or truncated Huffman symbol");
    b.bits >>= n;
    b.count -= n;
    return h.symbols[index];
}
static uint32_t adler32(View data) {
    uint32_t a = 1, b = 0;
    size_t i = 0;
    while (i < data.size()) {
        size_t end = std::min(data.size(), i + 5552);
        for (; i < end; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}
Bytes inflate(View input, size_t limit) {
    require(input.size() >= 6, "Truncated zlib stream");
    require((input[0] & 15) == 8 && (input[0] >> 4) <= 7 && ((unsigned(input[0]) * 256 + input[1]) % 31) == 0,
            "Invalid zlib header");
    require(!(input[1] & 32), "Preset DEFLATE dictionaries are not supported by PDF");
    Bits b{input.subspan(2, input.size() - 6)};
    Bytes out;
    out.reserve(std::min(limit, input.size() * 3));
    bool final = false;
    static constexpr unsigned length_base[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                               15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                               67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr unsigned length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr unsigned dist_base[] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                             33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr unsigned dist_extra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const Huffman fixed_lit = [] {
        std::array<uint8_t, 288> l{};
        for (int i = 0; i < 288; ++i)
            l[i] = uint8_t(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        return huffman(l);
    }();
    static const Huffman fixed_dist = [] {
        std::array<uint8_t, 32> l{};
        l.fill(5);
        return huffman(l);
    }();
    while (!final) {
        final = take(b, 1) != 0;
        unsigned type = take(b, 2);
        if (type == 0) {
            take(b, b.count % 8);
            unsigned count = take(b, 16), other = take(b, 16);
            require((count ^ 0xffff) == other, "Invalid stored DEFLATE length");
            require(count <= limit - out.size(), "Decompressed stream limit exceeded");
            for (unsigned i = 0; i < count; ++i)
                out.push_back(uint8_t(take(b, 8)));
            continue;
        }
        require(type != 3, "Reserved DEFLATE block type");
        std::unique_ptr<Huffman> lit, dist;
        const Huffman* lh = &fixed_lit;
        const Huffman* dh = &fixed_dist;
        if (type == 2) {
            lit = std::make_unique<Huffman>();
            dist = std::make_unique<Huffman>();
            unsigned nl = take(b, 5) + 257, nd = take(b, 5) + 1, nc = take(b, 4) + 4;
            require(nl <= 286 && nd <= 32, "Invalid dynamic Huffman counts");
            // RFC 1951 code-length alphabet order.
            constexpr unsigned permutation[] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
            std::array<uint8_t, 19> cl{};
            for (unsigned i = 0; i < nc; ++i)
                cl[permutation[i]] = uint8_t(take(b, 3));
            auto ch = std::make_unique<Huffman>(huffman(cl));
            std::vector<uint8_t> lengths;
            lengths.reserve(nl + nd);
            while (lengths.size() < nl + nd) {
                unsigned s = symbol(b, *ch);
                if (s < 16)
                    lengths.push_back(uint8_t(s));
                else {
                    unsigned count = 0;
                    uint8_t value = 0;
                    if (s == 16) {
                        require(!lengths.empty(), "Huffman repeat has no previous value");
                        value = lengths.back();
                        count = take(b, 2) + 3;
                    } else if (s == 17)
                        count = take(b, 3) + 3;
                    else if (s == 18)
                        count = take(b, 7) + 11;
                    else
                        fail("Invalid Huffman repeat");
                    require(count <= nl + nd - lengths.size(), "Huffman repeat exceeds alphabet");
                    lengths.insert(lengths.end(), count, value);
                }
            }
            require(lengths[256] != 0, "Huffman tree is missing end-of-block");
            *lit = huffman(std::span(lengths).first(nl));
            *dist = huffman(std::span(lengths).subspan(nl));
            lh = lit.get();
            dh = dist.get();
        }
        for (;;) {
            unsigned s = symbol(b, *lh);
            if (s < 256) {
                require(out.size() < limit, "Decompressed stream limit exceeded");
                out.push_back(uint8_t(s));
                continue;
            }
            if (s == 256)
                break;
            require(s <= 285, "Invalid DEFLATE length code");
            unsigned n = length_base[s - 257] + take(b, length_extra[s - 257]);
            unsigned dc = symbol(b, *dh);
            require(dc < 30, "Invalid DEFLATE distance code");
            unsigned distance = dist_base[dc] + take(b, dist_extra[dc]);
            require(distance <= out.size() && n <= limit - out.size(),
                    "Invalid DEFLATE back-reference or output limit");
            size_t base = out.size();
            out.resize(base + n);
            if (distance >= n)
                std::memcpy(out.data() + base, out.data() + base - distance, n);
            else if (distance == 1)
                std::memset(out.data() + base, out[base - 1], n);
            else {
                // Copy one period, then double the initialized prefix. Each
                // copy has disjoint ranges, including overlapping PDF runs.
                std::memcpy(out.data() + base, out.data() + base - distance, distance);
                for (unsigned copied = distance; copied < n;) {
                    unsigned count = std::min(copied, n - copied);
                    std::memcpy(out.data() + base + copied, out.data() + base, count);
                    copied += count;
                }
            }
        }
    }
    // PDF streams may contain padding after the zlib end marker. The checksum
    // follows the final DEFLATE block, not necessarily the stream's last byte.
    size_t checksum = 2 + b.pos - b.count / 8;
    require(checksum <= input.size() && input.size() - checksum >= 4, "Missing zlib checksum");
    uint32_t expected = 0;
    for (auto n : input.subspan(checksum, 4))
        expected = (expected << 8) | n;
    require(adler32(out) == expected, "Zlib checksum mismatch");
    return out;
}
Bytes ascii_hex(View data) {
    Bytes out;
    int high = -1;
    for (uint8_t c : data) {
        if (c == '>')
            break;
        if (whitespace(c))
            continue;
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        require(v >= 0, "Invalid ASCIIHex data");
        if (high < 0)
            high = v;
        else {
            require(out.size() < max_stream, "ASCIIHex size limit exceeded");
            out.push_back(uint8_t(high * 16 + v));
            high = -1;
        }
    }
    if (high >= 0)
        out.push_back(uint8_t(high * 16));
    return out;
}
Bytes ascii85(View data) {
    Bytes out;
    uint64_t accum = 0;
    unsigned count = 0;
    size_t i = 0;
    if (data.size() >= 2 && data[0] == '<' && data[1] == '~')
        i = 2;
    for (; i < data.size(); ++i) {
        uint8_t c = data[i];
        if (whitespace(c))
            continue;
        if (c == '~') {
            require(i + 1 < data.size() && data[i + 1] == '>', "Invalid ASCII85 terminator");
            break;
        }
        if (c == 'z') {
            require(count == 0, "ASCII85 z inside a tuple");
            require(out.size() <= max_stream - 4, "ASCII85 size limit exceeded");
            out.insert(out.end(), 4, 0);
            continue;
        }
        require(c >= '!' && c <= 'u', "Invalid ASCII85 digit");
        accum = accum * 85 + c - '!';
        if (++count == 5) {
            require(accum <= UINT32_MAX && out.size() <= max_stream - 4, "ASCII85 tuple overflow");
            for (int j = 3; j >= 0; --j)
                out.push_back(uint8_t(accum >> (j * 8)));
            count = 0;
            accum = 0;
        }
    }
    require(count != 1, "Incomplete ASCII85 tuple");
    if (count) {
        for (unsigned n = count; n < 5; ++n)
            accum = accum * 85 + 84;
        require(accum <= UINT32_MAX && out.size() <= max_stream - 4, "ASCII85 tuple overflow");
        for (unsigned n = 0; n < count - 1; ++n)
            out.push_back(uint8_t(accum >> (24 - n * 8)));
    }
    return out;
}
Bytes run_length(View data) {
    Bytes out;
    size_t i = 0;
    while (i < data.size()) {
        unsigned n = data[i++];
        if (n == 128)
            return out;
        if (n < 128) {
            ++n;
            require(n <= data.size() - i && n <= max_stream - out.size(), "Invalid RunLength literal");
            out.insert(out.end(), data.begin() + i, data.begin() + i + n);
            i += n;
        } else {
            n = 257 - n;
            require(i < data.size() && n <= max_stream - out.size(), "Invalid RunLength repeat");
            out.insert(out.end(), n, data[i++]);
        }
    }
    fail("Missing RunLength end marker");
}
struct MsbBits {
    View data;
    size_t bit = 0;
};
static unsigned msb(MsbBits& b, unsigned n) {
    require(n <= 24 && b.bit <= b.data.size() * 8 && n <= b.data.size() * 8 - b.bit, "Truncated bitstream");
    unsigned v = 0;
    for (unsigned i = 0; i < n; ++i) {
        v = (v << 1) | ((b.data[b.bit / 8] >> (7 - b.bit % 8)) & 1);
        ++b.bit;
    }
    return v;
}
Bytes lzw(View data, int early) {
    require(early == 0 || early == 1, "Invalid LZW EarlyChange");
    MsbBits bits{data};
    std::array<uint16_t, 4096> prefix{};
    std::array<uint8_t, 4096> suffix{}, stack{};
    Bytes out;
    unsigned width = 9, next = 258;
    int previous = -1;
    uint8_t first = 0;
    for (;;) {
        unsigned code = msb(bits, width);
        if (code == 257)
            break;
        if (code == 256) {
            width = 9;
            next = 258;
            previous = -1;
            continue;
        }
        unsigned current = code, used = 0;
        require(code < next || (code == next && previous >= 0), "Invalid LZW code");
        if (current == next) {
            stack[used++] = first;
            current = unsigned(previous);
        }
        while (current >= 256) {
            require(current < next && used < 4095, "Cyclic LZW dictionary");
            stack[used++] = suffix[current];
            current = prefix[current];
        }
        first = uint8_t(current);
        stack[used++] = first;
        require(used <= max_stream - out.size(), "LZW output limit exceeded");
        while (used)
            out.push_back(stack[--used]);
        if (previous >= 0 && next < 4096) {
            prefix[next] = uint16_t(previous);
            suffix[next] = first;
            ++next;
            if (width < 12 && next + unsigned(early) == (1u << width))
                ++width;
        }
        previous = int(code);
    }
    return out;
}
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
Bytes predictor(Bytes data, const Value& params) {
    int type = integer(get(params, "Predictor"), 1);
    if (type <= 1)
        return data;
    int columns = integer(get(params, "Columns"), 1), colors = integer(get(params, "Colors"), 1),
        bits = integer(get(params, "BitsPerComponent"), 8);
    require(columns > 0 && colors > 0 && colors <= 32 &&
                (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16),
            "Invalid predictor geometry");
    uint64_t row_bits = uint64_t(columns) * colors * bits;
    require(row_bits <= max_stream * 8ull, "Predictor row limit exceeded");
    size_t row = size_t((row_bits + 7) / 8), bpp = size_t((colors * bits + 7) / 8);
    if (type == 2) {
        require(data.size() % row == 0, "Truncated TIFF predictor row");
        unsigned mask = (1u << bits) - 1;
        for (size_t offset = 0; offset < data.size(); offset += row) {
            if (bits == 8) {
                for (size_t j = size_t(colors); j < row; ++j)
                    data[offset + j] = uint8_t(data[offset + j] + data[offset + j - colors]);
            } else {
                std::array<unsigned, 32> previous{};
                for (size_t n = 0; n < size_t(columns) * colors; ++n) {
                    size_t bit = n * bits;
                    unsigned value = 0;
                    for (int k = 0; k < bits; ++k)
                        value = (value << 1) | ((data[offset + (bit + k) / 8] >> (7 - (bit + k) % 8)) & 1);
                    value = (value + previous[n % colors]) & mask;
                    previous[n % colors] = value;
                    for (int k = 0; k < bits; ++k) {
                        auto& byte = data[offset + (bit + k) / 8];
                        unsigned shift = unsigned(7 - (bit + k) % 8);
                        byte = uint8_t((byte & ~(1u << shift)) | (((value >> (bits - 1 - k)) & 1) << shift));
                    }
                }
            }
        }
        return data;
    }
    require(type >= 10 && type <= 15, "Unsupported stream predictor");
    require(data.size() % (row + 1) == 0, "Truncated PNG predictor row");
    size_t rows = data.size() / (row + 1);
    for (size_t y = 0; y < rows; ++y) {
        size_t src = y * (row + 1), dst = y * row;
        unsigned filter = data[src++];
        require(filter <= 4, "Invalid PNG predictor filter");
        // The input stays ahead of the output, so rows can be decoded in place.
        if (filter == 0 || (filter == 2 && !y)) {
            std::memmove(data.data() + dst, data.data() + src, row);
            continue;
        }
        if (filter == 2) {
            size_t x = 0;
            for (; x + 16 <= row; x += 16) {
                auto encoded = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data.data() + src + x));
                auto previous =
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(data.data() + dst + x - row));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(data.data() + dst + x),
                                 _mm_add_epi8(encoded, previous));
            }
            for (; x < row; ++x)
                data[dst + x] = uint8_t(data[src + x] + data[dst + x - row]);
            continue;
        }
        for (size_t x = 0; x < row; ++x) {
            int a = x >= bpp ? data[dst + x - bpp] : 0, b = y ? data[dst + x - row] : 0,
                c = y && x >= bpp ? data[dst + x - row - bpp] : 0;
            int prediction = filter == 1 ? a : filter == 3 ? (a + b) / 2 : paeth(a, b, c);
            data[dst + x] = uint8_t(data[src + x] + prediction);
        }
    }
    data.resize(rows * row);
    return data;
}
DecodeResult decode_stream(Document& d, const Value& input, bool image) {
    const auto& s = resolve(d, input);
    require(s.has_stream, "Expected a PDF stream");
    check_cancel(d);
    require(s.stream.size() <= max_stream, "Encoded stream limit exceeded");
    const auto& filters = entry(d, s, "Filter");
    const auto& parms = entry(d, s, "DecodeParms");
    std::vector<const Value*> list;
    if (filters.kind == Kind::array)
        for (auto& f : filters.array)
            list.push_back(&resolve(d, f));
    else if (filters.kind != Kind::null)
        list.push_back(&filters);
    bool explicit_crypt = std::any_of(list.begin(), list.end(), [](auto f) { return named(*f, "Crypt"); });
    Bytes bytes;
    if (d.security.enabled && !explicit_crypt && !named(get(s, "Type"), "XRef") &&
        (d.security.metadata || !named(get(s, "Type"), "Metadata")))
        bytes = decrypt(d, s.stream, s.object, s.generation, d.security.stream_method);
    else
        bytes.assign(s.stream.begin(), s.stream.end());
    for (size_t i = 0; i < list.size(); ++i) {
        check_cancel(d);
        const auto& f = *list[i];
        const auto& p = resolve(d, parms.kind == Kind::array ? at(parms, i) : parms);
        auto n = f.text;
        if (n == "FlateDecode" || n == "Fl")
            bytes = predictor(inflate(bytes), p);
        else if (n == "LZWDecode" || n == "LZW")
            bytes = predictor(lzw(bytes, integer(get(p, "EarlyChange"), 1)), p);
        else if (n == "ASCIIHexDecode" || n == "AHx")
            bytes = ascii_hex(bytes);
        else if (n == "ASCII85Decode" || n == "A85")
            bytes = ascii85(bytes);
        else if (n == "RunLengthDecode" || n == "RL")
            bytes = run_length(bytes);
        else if (n == "Crypt") {
            auto name = get(p, "Name").text;
            std::string method = "Identity";
            if (!name.empty() && name != "Identity") {
                auto& enc = entry(d, d.trailer, "Encrypt");
                method = get(entry(d, entry(d, enc, "CF"), name), "CFM").text;
            }
            bytes = decrypt(d, bytes, s.object, s.generation, method);
        } else if (image && (n == "DCTDecode" || n == "DCT" || n == "JPXDecode" || n == "CCITTFaxDecode" ||
                             n == "CCF" || n == "JBIG2Decode")) {
            require(i + 1 == list.size(), "Image codec must be the final filter");
            return {std::move(bytes), std::move(n), p};
        } else
            fail("Unsupported stream filter: " + n);
    }
    return {std::move(bytes), {}, {}};
}
} // namespace pdf
