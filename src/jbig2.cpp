#include "image_codecs.h"
#include <optional>

namespace pdf {
namespace jbig {
struct Reader {
    View data;
    size_t position = 0;
    unsigned bit = 0;
};
static uint32_t read(Reader& r, unsigned bytes) {
    require(!r.bit && bytes <= 4 && r.position <= r.data.size() && bytes <= r.data.size() - r.position,
            "Truncated JBIG2 data");
    uint32_t v = 0;
    while (bytes--)
        v = (v << 8) | r.data[r.position++];
    return v;
}
static uint32_t bits(Reader& r, unsigned count) {
    require(count <= 32, "Invalid JBIG2 bit count");
    uint32_t v = 0;
    while (count--) {
        require(r.position < r.data.size(), "Truncated JBIG2 Huffman data");
        v = (v << 1) | ((r.data[r.position] >> (7 - r.bit)) & 1);
        if (++r.bit == 8) {
            r.bit = 0;
            ++r.position;
        }
    }
    return v;
}
static void align(Reader& r) {
    if (r.bit) {
        r.bit = 0;
        ++r.position;
    }
}
static View take(Reader& r, size_t length) {
    require(!r.bit && r.position <= r.data.size() && length <= r.data.size() - r.position,
            "Truncated JBIG2 bitmap");
    auto result = r.data.subspan(r.position, length);
    r.position += length;
    return result;
}
struct Offset {
    int x = 0, y = 0;
};
struct Code {
    unsigned length = 0, extra = 0;
    int64_t base = 0;
    bool lower = false, oob = false;
    uint32_t code = 0;
};
using Huffman = std::vector<Code>;
#include "jbig2_tables.inc"
static void assign_codes(Huffman& table) {
    std::array<uint64_t, 33> counts{}, next{};
    for (auto& c : table) {
        require(c.length <= 32 && c.extra <= 32, "JBIG2 Huffman length limit");
        if (c.length)
            ++counts[c.length];
    }
    for (unsigned i = 1; i <= 32; ++i) {
        next[i] = (next[i - 1] + counts[i - 1]) * 2;
        require(next[i] + counts[i] <= (uint64_t(1) << i), "Oversubscribed JBIG2 Huffman table");
    }
    for (auto& c : table)
        if (c.length)
            c.code = uint32_t(next[c.length]++);
}
static std::optional<int64_t> huffman(Reader& r, const Huffman& table) {
    uint32_t prefix = 0;
    for (unsigned length = 1; length <= 32; ++length) {
        prefix = (prefix << 1) | bits(r, 1);
        for (auto& c : table)
            if (c.length == length && c.code == prefix) {
                if (c.oob)
                    return {};
                int64_t offset = bits(r, c.extra);
                return c.lower ? c.base - offset : c.base + offset;
            }
    }
    fail("Invalid JBIG2 Huffman code");
}
static Huffman custom_table(Reader& r) {
    unsigned flags = read(r, 1), prefix_bits = ((flags >> 1) & 7) + 1, range_bits = ((flags >> 4) & 7) + 1;
    int64_t low = int32_t(read(r, 4)), high = int32_t(read(r, 4));
    require(low < high, "Invalid JBIG2 custom table range");
    Huffman table;
    for (int64_t value = low; value < high;) {
        require(table.size() < 65536, "JBIG2 custom table size limit");
        unsigned length = bits(r, prefix_bits), extra = bits(r, range_bits);
        require(extra <= 32, "JBIG2 custom table range overflow");
        table.push_back({length, extra, value});
        value += int64_t(1) << extra;
    }
    table.push_back({bits(r, prefix_bits), 32, low - 1, true});
    table.push_back({bits(r, prefix_bits), 32, high});
    if (flags & 1)
        table.push_back({bits(r, prefix_bits), 0, 0, false, true});
    assign_codes(table);
    return table;
}
enum IntegerContext { DH, DW, EX, AI, DT, FS, DS, IT, RI, RDW, RDH, RDX, RDY, context_count };
struct Arithmetic {
    MQDecoder mq;
    std::array<std::array<uint8_t, 512>, context_count> integers{};
    Bytes generic = Bytes(65536), refinement = Bytes(8192), ids;
};
static std::optional<int64_t> arithmetic_integer(Arithmetic& a, IntegerContext which) {
    unsigned previous = 1;
    auto bit = [&] {
        unsigned value = mq_bit(a.mq, a.integers[which][previous]);
        previous = previous < 256 ? (previous << 1) | value : (((previous << 1) | value) & 511) | 256;
        return value;
    };
    unsigned negative = bit(), count = 2;
    int64_t offset = 0;
    if (bit()) {
        count = 4;
        offset = 4;
        if (bit()) {
            count = 6;
            offset = 20;
            if (bit()) {
                count = 8;
                offset = 84;
                if (bit()) {
                    count = 12;
                    offset = 340;
                    if (bit()) {
                        count = 32;
                        offset = 4436;
                    }
                }
            }
        }
    }
    int64_t value = 0;
    while (count--)
        value = (value << 1) | bit();
    value += offset;
    if (negative && value == 0)
        return {};
    return negative ? -value : value;
}
static unsigned id_bits(size_t count) {
    unsigned length = 0;
    for (size_t n = count ? count - 1 : 0; n; n >>= 1)
        ++length;
    require(length <= 20, "JBIG2 symbol count limit");
    return length;
}
static unsigned symbol_id(Arithmetic& a, unsigned length) {
    size_t needed = size_t(1) << (length + 1);
    if (a.ids.size() < needed)
        a.ids.resize(needed);
    unsigned previous = 1;
    for (unsigned i = 0; i < length; ++i)
        previous = (previous << 1) | mq_bit(a.mq, a.ids[previous]);
    return previous - (1u << length);
}
struct Bitmap {
    int width = 0, height = 0;
    Bytes pixels;
};
using BitmapPtr = std::shared_ptr<Bitmap>;
struct Segment {
    uint32_t number = 0, page = 0, length = 0;
    int actual_rows = -1;
    unsigned type = 0;
    std::vector<uint32_t> references;
    View data;
    std::vector<BitmapPtr> symbols;
    BitmapPtr bitmap;
    Huffman table;
    Bytes generic, refinement;
    int x = 0, y = 0;
    unsigned operation = 0;
};
struct Decoder {
    Renderer& renderer;
    Document& document;
    unsigned expected_width = 0, expected_height = 0, page_flags = 0;
    size_t allocation = 0;
    std::unordered_map<uint32_t, Segment> segments;
    BitmapPtr page;
};
static BitmapPtr bitmap(Decoder& d, int64_t width, int64_t height, unsigned value = 0) {
    require(width >= 0 && height >= 0 && width <= 1000000 && height <= 1000000 &&
                uint64_t(width) * height <= max_stream / 4,
            "JBIG2 bitmap dimensions exceed limit");
    size_t size = size_t(width) * size_t(height);
    require(d.allocation + size <= size_t(max_stream) * 2, "JBIG2 aggregate allocation limit");
    d.allocation += size;
    auto b = std::make_shared<Bitmap>();
    b->width = int(width);
    b->height = int(height);
    b->pixels.assign(size, uint8_t(value));
    return b;
}
static unsigned pixel(const Bitmap& b, int64_t x, int64_t y) {
    return x >= 0 && y >= 0 && x < b.width && y < b.height ? b.pixels[size_t(y) * b.width + size_t(x)] : 0;
}
static void combine(Bitmap& dst, const Bitmap& src, int64_t x, int64_t y, unsigned operation) {
    require(operation <= 4, "Invalid JBIG2 combination operator");
    int64_t x0 = std::max<int64_t>(0, -x), x1 = std::min<int64_t>(src.width, dst.width - x),
            y0 = std::max<int64_t>(0, -y), y1 = std::min<int64_t>(src.height, dst.height - y);
    for (int64_t yy = y0; yy < y1; ++yy)
        for (int64_t xx = x0; xx < x1; ++xx) {
            auto& p = dst.pixels[size_t(yy + y) * dst.width + size_t(xx + x)];
            unsigned q = src.pixels[size_t(yy) * src.width + size_t(xx)];
            p = uint8_t(operation == 0   ? p | q
                        : operation == 1 ? p & q
                        : operation == 2 ? p ^ q
                        : operation == 3 ? 1 ^ (p ^ q)
                                         : q);
        }
}
static BitmapPtr mmr(Decoder& d, View data, int width, int height) {
    auto b = bitmap(d, width, height);
    if (width && height) {
        auto out = decode_mmr(d.renderer, data, unsigned(width), unsigned(height));
        b->pixels = std::move(out.pixels);
    }
    return b;
}
static std::vector<Offset> adaptive(Reader& r, unsigned count) {
    std::vector<Offset> offsets;
    while (count--) {
        int x = int8_t(read(r, 1)), y = int8_t(read(r, 1));
        offsets.push_back({x, y});
    }
    return offsets;
}
static std::vector<Offset> generic_template(unsigned kind, const std::vector<Offset>& at) {
    require(kind < 4 && at.size() >= (kind ? 1u : 4u), "Missing JBIG2 adaptive template");
    std::vector<Offset> result;
    if (kind == 0)
        result = {at[3],   {-1, -2}, {0, -2}, {1, -2}, at[2],   at[1],   {-2, -1}, {-1, -1},
                  {0, -1}, {1, -1},  {2, -1}, at[0],   {-4, 0}, {-3, 0}, {-2, 0},  {-1, 0}};
    else if (kind == 1)
        result = {{-1, -2}, {0, -2}, {1, -2}, {2, -2}, {-2, -1}, {-1, -1}, {0, -1},
                  {1, -1},  {2, -1}, at[0],   {-3, 0}, {-2, 0},  {-1, 0}};
    else if (kind == 2)
        result = {{-1, -2}, {0, -2}, {1, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, at[0], {-2, 0}, {-1, 0}};
    else
        result = {{-3, -1}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, at[0], {-4, 0}, {-3, 0}, {-2, 0}, {-1, 0}};
    for (auto p : result)
        require(p.y < 0 || (p.y == 0 && p.x < 0), "JBIG2 adaptive pixel does not precede current pixel");
    return result;
}
static BitmapPtr generic(Decoder& d, Arithmetic& a, int width, int height, unsigned kind,
                         const std::vector<Offset>& at, bool typical, const Bitmap* skip = nullptr) {
    auto out = bitmap(d, width, height);
    auto offsets = generic_template(kind, at);
    constexpr unsigned typical_context[] = {0x9b25, 0x795, 0xe5, 0x195};
    unsigned ltp = 0;
    for (int y = 0; y < height; ++y) {
        if ((y & 63) == 0)
            check_cancel(d.document);
        if (typical)
            ltp ^= mq_bit(a.mq, a.generic[typical_context[kind]]);
        if (ltp) {
            if (y)
                std::copy_n(out->pixels.begin() + size_t(y - 1) * width, width,
                            out->pixels.begin() + size_t(y) * width);
            continue;
        }
        for (int x = 0; x < width; ++x) {
            if (skip && pixel(*skip, x, y))
                continue;
            unsigned context = 0;
            for (auto p : offsets)
                context = (context << 1) | pixel(*out, x + p.x, y + p.y);
            out->pixels[size_t(y) * width + x] = uint8_t(mq_bit(a.mq, a.generic[context]));
        }
    }
    return out;
}
static BitmapPtr refine(Decoder& d, Arithmetic& a, int width, int height, unsigned kind,
                        const std::vector<Offset>& at, const Bitmap& reference, int64_t dx, int64_t dy,
                        bool typical = false) {
    require(kind < 2 && (kind || at.size() == 2), "Invalid JBIG2 refinement template");
    auto out = bitmap(d, width, height);
    std::vector<Offset> target = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}};
    std::vector<Offset> source = kind ? std::vector<Offset>{{0, -1}, {-1, 0}, {0, 0}, {1, 0}, {0, 1}, {1, 1}}
                                      : std::vector<Offset>{{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0},
                                                            {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
    if (!kind) {
        target[0] = at[0];
        source[0] = at[1];
        require(at[0].y < 0 || (at[0].y == 0 && at[0].x < 0), "Invalid JBIG2 refinement adaptive target");
    }
    unsigned ltp = 0;
    for (int y = 0; y < height; ++y) {
        if ((y & 63) == 0)
            check_cancel(d.document);
        if (typical)
            ltp ^= mq_bit(a.mq, a.refinement[kind ? 8 : 16]);
        for (int x = 0; x < width; ++x) {
            unsigned value = pixel(reference, x - dx, y - dy);
            bool implicit = ltp != 0;
            if (implicit)
                for (int yy = -1; yy <= 1; ++yy)
                    for (int xx = -1; xx <= 1; ++xx)
                        if (pixel(reference, x - dx + xx, y - dy + yy) != value)
                            implicit = false;
            if (!implicit) {
                unsigned context = 0;
                for (auto p : target)
                    context = (context << 1) | pixel(*out, x + p.x, y + p.y);
                for (auto p : source)
                    context = (context << 1) | pixel(reference, x - dx + p.x, y - dy + p.y);
                value = mq_bit(a.mq, a.refinement[context]);
            }
            out->pixels[size_t(y) * width + x] = uint8_t(value);
        }
    }
    return out;
}
static int bounded(int64_t value) {
    require(value >= -1000000 && value <= 1000000, "JBIG2 coordinate or dimension limit");
    return int(value);
}
static int64_t number(std::optional<int64_t> v) {
    require(v.has_value(), "Unexpected JBIG2 out-of-band integer");
    return *v;
}
static Segment& referred(Decoder& d, uint32_t id) {
    auto it = d.segments.find(id);
    require(it != d.segments.end(), "Missing JBIG2 referred segment");
    return it->second;
}
static std::vector<BitmapPtr> symbols(Decoder& d, const Segment& segment) {
    std::vector<BitmapPtr> result;
    for (auto id : segment.references) {
        auto& ref = referred(d, id);
        if (ref.type == 0)
            result.insert(result.end(), ref.symbols.begin(), ref.symbols.end());
    }
    require(result.size() <= 1000000, "JBIG2 imported symbol limit");
    return result;
}
static const Huffman& select_table(Decoder& d, const Segment& segment, size_t& custom, unsigned selection,
                                   std::initializer_list<unsigned> standard) {
    if (selection == 3) {
        while (custom < segment.references.size()) {
            auto& ref = referred(d, segment.references[custom++]);
            if (ref.type == 53)
                return ref.table;
        }
        fail("Missing JBIG2 custom Huffman table");
    }
    require(selection < standard.size(), "Invalid JBIG2 Huffman table selection");
    auto it = standard.begin();
    std::advance(it, selection);
    return standard_tables[*it];
}
struct TextParameters {
    unsigned flags = 0, instances = 0, symbol_bits = 0;
    bool huff = false;
    std::vector<Offset> at;
    Huffman ids;
    std::array<Huffman, 8> tables;
};
static std::optional<int64_t> integer(Arithmetic& a, Reader& r, bool huff, IntegerContext which,
                                      const Huffman& table) {
    return huff ? huffman(r, table) : arithmetic_integer(a, which);
}
static BitmapPtr refined_bitmap(Decoder& d, Arithmetic& a, Reader& r, bool huff, int width, int height,
                                unsigned kind, const std::vector<Offset>& at, const Bitmap& base, int64_t dx,
                                int64_t dy, const Huffman& size_table) {
    if (!huff)
        return refine(d, a, width, height, kind, at, base, dx, dy);
    int64_t length = number(huffman(r, size_table));
    require(length >= 0 && length <= max_stream, "Invalid JBIG2 refinement length");
    align(r);
    auto bytes = take(r, size_t(length));
    MQDecoder saved = a.mq;
    mq_init(a.mq, bytes);
    auto result = refine(d, a, width, height, kind, at, base, dx, dy);
    a.mq = saved;
    return result;
}
static BitmapPtr text_region(Decoder& d, Arithmetic& a, Reader& r, int width, int height,
                             const std::vector<BitmapPtr>& syms, const TextParameters& p) {
    auto out = bitmap(d, width, height, (p.flags >> 9) & 1);
    unsigned strips = 1u << ((p.flags >> 2) & 3), corner = (p.flags >> 4) & 3, operation = (p.flags >> 7) & 3;
    bool transpose = (p.flags & 64) != 0, right = (corner & 2) != 0, bottom = (corner & 1) == 0;
    int delta = int((p.flags >> 10) & 31);
    if (delta & 16)
        delta -= 32;
    auto val = [&](IntegerContext ctx, unsigned table) {
        return integer(a, r, p.huff, ctx, p.tables[table]);
    };
    int64_t strip = -number(val(DT, 2)) * strips, first = 0;
    unsigned done = 0;
    require(p.instances <= 10000000, "JBIG2 text instance limit");
    while (done < p.instances) {
        check_cancel(d.document);
        strip += number(val(DT, 2)) * strips;
        bounded(strip);
        first += number(val(FS, 0));
        bounded(first);
        int64_t s = first;
        bool first_in_strip = true;
        while (true) {
            if (!first_in_strip) {
                auto distance = val(DS, 1);
                if (!distance)
                    break;
                s += *distance + delta;
            }
            first_in_strip = false;
            bounded(s);
            require(done < p.instances, "JBIG2 text instance count mismatch");
            int64_t t = strip + (strips == 1 ? 0
                                 : p.huff    ? bits(r, (p.flags >> 2) & 3)
                                             : number(arithmetic_integer(a, IT)));
            bounded(t);
            unsigned id = p.huff ? (p.ids.empty() ? bits(r, std::max(1u, p.symbol_bits))
                                                  : unsigned(number(huffman(r, p.ids))))
                                 : symbol_id(a, p.symbol_bits);
            require(id < syms.size() && syms[id], "Invalid JBIG2 symbol identifier");
            auto symbol = syms[id];
            bool refinement = (p.flags & 2) && (p.huff ? bits(r, 1) : number(arithmetic_integer(a, RI)));
            if (refinement) {
                int dw = bounded(number(val(RDW, 3))), dh = bounded(number(val(RDH, 4))),
                    dx = bounded(number(val(RDX, 5))), dy = bounded(number(val(RDY, 6)));
                symbol = refined_bitmap(d, a, r, p.huff, bounded(int64_t(symbol->width) + dw),
                                        bounded(int64_t(symbol->height) + dh), p.flags >> 15, p.at, *symbol,
                                        int64_t(std::floor(dw / 2.0)) + dx,
                                        int64_t(std::floor(dh / 2.0)) + dy, p.tables[7]);
            }
            if ((!transpose && right) || (transpose && bottom))
                s += (transpose ? symbol->height : symbol->width) - 1;
            int64_t x = transpose ? t : s, y = transpose ? s : t;
            if (right)
                x -= symbol->width - 1;
            if (bottom)
                y -= symbol->height - 1;
            combine(*out, *symbol, x, y, operation);
            if ((!transpose && !right) || (transpose && !bottom))
                s += (transpose ? symbol->height : symbol->width) - 1;
            ++done;
        }
    }
    return out;
}
static Huffman symbol_table(Reader& r, size_t count) {
    Huffman runs;
    for (unsigned i = 0; i < 35; ++i)
        runs.push_back({bits(r, 4), 0, int64_t(i)});
    assign_codes(runs);
    Huffman table;
    while (table.size() < count) {
        unsigned code = unsigned(number(huffman(r, runs)));
        if (code < 32)
            table.push_back({code, 0, int64_t(table.size())});
        else {
            unsigned length = 0, repeat = 0;
            if (code == 32) {
                require(!table.empty(), "JBIG2 repeat without preceding symbol");
                length = table.back().length;
                repeat = bits(r, 2) + 3;
            } else if (code == 33)
                repeat = bits(r, 3) + 3;
            else {
                require(code == 34, "Invalid JBIG2 symbol run");
                repeat = bits(r, 7) + 11;
            }
            require(repeat <= count - table.size(), "JBIG2 symbol run exceeds table");
            while (repeat--)
                table.push_back({length, 0, int64_t(table.size())});
        }
    }
    align(r);
    assign_codes(table);
    return table;
}
static void dictionary(Decoder& d, Segment& segment, Reader& r) {
    unsigned flags = read(r, 2), kind = (flags >> 10) & 3, ref_kind = (flags >> 12) & 1;
    bool huff = (flags & 1) != 0, aggregate = (flags & 2) != 0;
    auto at = huff ? std::vector<Offset>{} : adaptive(r, kind ? 1 : 4);
    auto rat = aggregate && !ref_kind ? adaptive(r, 2) : std::vector<Offset>{};
    unsigned exported = read(r, 4), new_count = read(r, 4);
    require(new_count <= 1000000 && exported <= 1000000, "JBIG2 symbol dictionary limit");
    auto syms = symbols(d, segment);
    size_t initial = syms.size();
    unsigned symbol_length = id_bits(initial + new_count);
    size_t custom = 0;
    Huffman dh, dw, size, agg;
    if (huff) {
        dh = select_table(d, segment, custom, (flags >> 2) & 3, {4, 5});
        dw = select_table(d, segment, custom, (flags >> 4) & 3, {2, 3});
        size = select_table(d, segment, custom, (flags & 64) ? 3 : 0, {1});
        agg = select_table(d, segment, custom, (flags & 128) ? 3 : 0, {1});
    }
    Arithmetic a;
    if (flags & 256) {
        bool found = false;
        for (auto id : segment.references) {
            auto& ref = referred(d, id);
            if (ref.type == 0 && !ref.generic.empty()) {
                a.generic = ref.generic;
                a.refinement = ref.refinement;
                found = true;
            }
        }
        require(found, "Missing retained JBIG2 bitmap contexts");
    }
    if (!huff)
        mq_init(a.mq, r.data.subspan(r.position));
    int height = 0;
    unsigned created = 0;
    while (created < new_count) {
        check_cancel(d.document);
        height = bounded(int64_t(height) + number(integer(a, r, huff, DH, dh)));
        require(height >= 0, "Negative JBIG2 symbol height");
        int width = 0, total = 0;
        unsigned first = created;
        std::vector<int> widths;
        while (true) {
            auto change = integer(a, r, huff, DW, dw);
            if (!change)
                break;
            require(created < new_count, "JBIG2 symbol count mismatch");
            width = bounded(int64_t(width) + *change);
            require(width >= 0, "Negative JBIG2 symbol width");
            total = bounded(int64_t(total) + width);
            BitmapPtr symbol;
            if (!aggregate && !huff)
                symbol = generic(d, a, width, height, kind, at, false);
            else if (aggregate) {
                int64_t instances = number(integer(a, r, huff, AI, agg));
                require(instances >= 1 && instances <= 10000000, "Invalid JBIG2 aggregate instance count");
                if (instances == 1) {
                    unsigned id = huff ? bits(r, std::max(1u, symbol_length)) : symbol_id(a, symbol_length);
                    require(id < syms.size() && syms[id], "Invalid JBIG2 refinement symbol ID");
                    int dx = bounded(number(integer(a, r, huff, RDX, standard_tables[15]))),
                        dy = bounded(number(integer(a, r, huff, RDY, standard_tables[15])));
                    symbol = refined_bitmap(d, a, r, huff, width, height, ref_kind, rat, *syms[id], dx, dy,
                                            standard_tables[1]);
                } else {
                    TextParameters p;
                    p.flags = 2 | 16 | (ref_kind << 15);
                    p.huff = huff;
                    p.instances = unsigned(instances);
                    p.symbol_bits = symbol_length;
                    p.at = rat;
                    p.tables = {standard_tables[6],  standard_tables[8],  standard_tables[11],
                                standard_tables[15], standard_tables[15], standard_tables[15],
                                standard_tables[15], standard_tables[1]};
                    symbol = text_region(d, a, r, width, height, syms, p);
                }
            }
            syms.push_back(std::move(symbol));
            widths.push_back(width);
            ++created;
        }
        require(created > first, "Empty JBIG2 height class");
        if (huff && !aggregate) {
            int64_t length = number(huffman(r, size));
            require(length >= 0 && length <= max_stream, "JBIG2 collective bitmap length limit");
            align(r);
            BitmapPtr collective;
            if (!length) {
                collective = bitmap(d, total, height);
                size_t stride = (size_t(total) + 7) / 8;
                auto raw = take(r, stride * height);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < total; ++x)
                        collective->pixels[size_t(y) * total + x] =
                            (raw[size_t(y) * stride + unsigned(x) / 8] >> (7 - (x & 7))) & 1;
            } else
                collective = mmr(d, take(r, size_t(length)), total, height);
            int x = 0;
            for (unsigned i = first; i < created; ++i) {
                int w = widths[i - first];
                auto symbol = bitmap(d, w, height);
                combine(*symbol, *collective, -x, 0, 4);
                syms[initial + i] = std::move(symbol);
                x += w;
            }
        }
    }
    size_t index = 0;
    bool flag = false;
    unsigned attempts = 0;
    while (index < syms.size()) {
        require(++attempts <= syms.size() * 2 + 2, "JBIG2 export run limit");
        int64_t run = number(integer(a, r, huff, EX, standard_tables[1]));
        require(run >= 0 && uint64_t(run) <= syms.size() - index, "Invalid JBIG2 symbol export run");
        if (flag)
            segment.symbols.insert(segment.symbols.end(), syms.begin() + index,
                                   syms.begin() + index + size_t(run));
        index += size_t(run);
        flag = !flag;
    }
    require(segment.symbols.size() == exported, "JBIG2 exported symbol count mismatch");
    if (flags & 512) {
        segment.generic = std::move(a.generic);
        segment.refinement = std::move(a.refinement);
    }
}
static void region_header(Segment& segment, Reader& r, int& width, int& height) {
    width = bounded(read(r, 4));
    height = bounded(read(r, 4));
    segment.x = bounded(read(r, 4));
    segment.y = bounded(read(r, 4));
    segment.operation = read(r, 1) & 7;
    require(width >= 0 && height >= 0 && segment.operation <= 4, "Invalid JBIG2 region header");
    if (segment.actual_rows >= 0) {
        require(segment.actual_rows <= height, "JBIG2 streamed row count exceeds region height");
        height = segment.actual_rows;
    }
}
static void decode_segment(Decoder& d, Segment& s) {
    check_cancel(d.document);
    Reader r{s.data};
    if (s.type == 0) {
        dictionary(d, s, r);
        return;
    }
    if (s.type == 53) {
        s.table = custom_table(r);
        return;
    }
    if (s.type == 48) {
        unsigned width = read(r, 4), height = read(r, 4);
        read(r, 4);
        read(r, 4);
        d.page_flags = read(r, 1);
        read(r, 2);
        if (height == UINT32_MAX)
            height = d.expected_height;
        require(width >= d.expected_width && width - d.expected_width <= 7 && height == d.expected_height,
                "JBIG2 page dimensions disagree with PDF image");
        d.page = bitmap(d, width, height, (d.page_flags >> 2) & 1);
        return;
    }
    if (s.type == 49 || s.type == 50 || s.type == 51 || s.type == 52 || s.type == 62)
        return;
    if (s.type == 16) {
        unsigned flags = read(r, 1), width = read(r, 1), height = read(r, 1), maximum = read(r, 4),
                 kind = (flags >> 1) & 3;
        require(width && height && maximum < 65536, "Invalid JBIG2 pattern dictionary");
        int total = bounded(int64_t(maximum + 1) * width);
        Arithmetic a;
        BitmapPtr collective;
        if (flags & 1)
            collective = mmr(d, r.data.subspan(r.position), total, int(height));
        else {
            mq_init(a.mq, r.data.subspan(r.position));
            std::vector<Offset> at = {{-int(width), 0}, {-3, -1}, {2, -2}, {-2, -2}};
            collective = generic(d, a, total, int(height), kind, at, false);
        }
        for (unsigned i = 0; i <= maximum; ++i) {
            auto pattern = bitmap(d, width, height);
            combine(*pattern, *collective, -int64_t(i) * width, 0, 4);
            s.symbols.push_back(std::move(pattern));
        }
        return;
    }
    int width = 0, height = 0;
    region_header(s, r, width, height);
    Arithmetic a;
    if (s.type == 36 || s.type == 38 || s.type == 39) {
        unsigned flags = read(r, 1), kind = (flags >> 1) & 3;
        require(!(flags & 0xf0), "Extended JBIG2 generic templates are not supported");
        if (flags & 1)
            s.bitmap = mmr(d, r.data.subspan(r.position), width, height);
        else {
            auto at = adaptive(r, kind ? 1 : 4);
            mq_init(a.mq, r.data.subspan(r.position));
            s.bitmap = generic(d, a, width, height, kind, at, (flags & 8) != 0);
        }
    } else if (s.type == 4 || s.type == 6 || s.type == 7) {
        auto syms = symbols(d, s);
        TextParameters p;
        p.flags = read(r, 2);
        p.huff = (p.flags & 1) != 0;
        unsigned flags = p.huff ? read(r, 2) : 0;
        if ((p.flags & 2) && !(p.flags & 0x8000))
            p.at = adaptive(r, 2);
        p.instances = read(r, 4);
        p.symbol_bits = id_bits(syms.size());
        if (p.huff) {
            size_t custom = 0;
            p.tables[0] = select_table(d, s, custom, flags & 3, {6, 7});
            p.tables[1] = select_table(d, s, custom, (flags >> 2) & 3, {8, 9, 10});
            p.tables[2] = select_table(d, s, custom, (flags >> 4) & 3, {11, 12, 13});
            for (unsigned i = 3; i <= 6; ++i)
                p.tables[i] = select_table(d, s, custom, (flags >> (i * 2)) & 3, {14, 15});
            p.tables[7] = select_table(d, s, custom, (flags & 0x4000) ? 3 : 0, {1});
            p.ids = symbol_table(r, syms.size());
        } else
            mq_init(a.mq, r.data.subspan(r.position));
        s.bitmap = text_region(d, a, r, width, height, syms, p);
    } else if (s.type == 40 || s.type == 42 || s.type == 43) {
        unsigned flags = read(r, 1);
        auto at = (flags & 1) ? std::vector<Offset>{} : adaptive(r, 2);
        BitmapPtr base;
        for (auto id : s.references)
            if (referred(d, id).bitmap) {
                require(!base, "JBIG2 refinement has multiple reference bitmaps");
                base = referred(d, id).bitmap;
            }
        if (!base) {
            require(bool(d.page), "JBIG2 refinement before page information");
            base = bitmap(d, width, height);
            combine(*base, *d.page, -s.x, -s.y, 4);
        }
        mq_init(a.mq, r.data.subspan(r.position));
        s.bitmap = refine(d, a, width, height, flags & 1, at, *base, 0, 0, (flags & 2) != 0);
    } else if (s.type == 20 || s.type == 22 || s.type == 23) {
        unsigned flags = read(r, 1), kind = (flags >> 1) & 3;
        int gw = bounded(read(r, 4)), gh = bounded(read(r, 4));
        int64_t gx = int32_t(read(r, 4)), gy = int32_t(read(r, 4));
        unsigned rx = read(r, 2), ry = read(r, 2);
        std::vector<BitmapPtr>* patterns = nullptr;
        for (auto id : s.references) {
            auto& ref = referred(d, id);
            if (ref.type == 16) {
                require(!patterns, "Multiple JBIG2 pattern dictionaries");
                patterns = &ref.symbols;
            }
        }
        require(patterns && !patterns->empty(), "Missing JBIG2 pattern dictionary");
        auto skip = bitmap(d, gw, gh);
        auto position = [&](int x, int y) {
            return std::pair{(gx + int64_t(y) * ry + int64_t(x) * rx) >> 8,
                             (gy + int64_t(y) * rx - int64_t(x) * ry) >> 8};
        };
        if (flags & 8)
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    auto [xx, yy] = position(x, y);
                    skip->pixels[size_t(y) * gw + x] =
                        uint8_t(xx + (*patterns)[0]->width <= 0 || yy + (*patterns)[0]->height <= 0 ||
                                xx >= width || yy >= height);
                }
        unsigned planes = id_bits(patterns->size());
        std::vector<uint32_t> gray(size_t(gw) * gh);
        BitmapPtr previous;
        if (!(flags & 1))
            mq_init(a.mq, r.data.subspan(r.position));
        for (unsigned plane = planes; plane-- > 0;) {
            BitmapPtr current;
            if (flags & 1) {
                size_t start = r.position;
                uint32_t tail = 0;
                bool found = false;
                while (r.position < r.data.size()) {
                    tail = ((tail << 1) | bits(r, 1)) & 0xffffff;
                    if (tail == 0x001001) {
                        found = true;
                        break;
                    }
                }
                require(found, "Missing JBIG2 MMR bit-plane terminator");
                align(r);
                current = mmr(d, r.data.subspan(start, r.position - start), gw, gh);
            } else {
                std::vector<Offset> at = {{kind <= 1 ? 3 : 2, -1}, {-3, -1}, {2, -2}, {-2, -2}};
                current = generic(d, a, gw, gh, kind, at, false, (flags & 8) ? skip.get() : nullptr);
            }
            for (size_t i = 0; i < gray.size(); ++i) {
                if (previous)
                    current->pixels[i] ^= previous->pixels[i];
                gray[i] |= unsigned(current->pixels[i]) << plane;
            }
            previous = std::move(current);
        }
        s.bitmap = bitmap(d, width, height, (flags >> 7) & 1);
        for (int y = 0; y < gh; ++y) {
            check_cancel(d.document);
            for (int x = 0; x < gw; ++x) {
                unsigned id = gray[size_t(y) * gw + x];
                require(id < patterns->size(), "Invalid JBIG2 halftone pattern index");
                auto [xx, yy] = position(x, y);
                combine(*s.bitmap, *(*patterns)[id], xx, yy, (flags >> 4) & 7);
            }
        }
    } else
        fail("Unsupported JBIG2 segment type: " + std::to_string(s.type));
    bool immediate = s.type == 6 || s.type == 7 || s.type == 22 || s.type == 23 || s.type == 38 ||
                     s.type == 39 || s.type == 42 || s.type == 43;
    if (immediate) {
        require(bool(d.page), "JBIG2 region precedes page information");
        unsigned operation =
            (d.page_flags & 64) || s.type == 42 || s.type == 43 ? s.operation : (d.page_flags >> 3) & 3;
        combine(*d.page, *s.bitmap, s.x, s.y, operation);
        s.bitmap.reset();
    }
}
static Segment segment_header(Reader& r) {
    Segment s;
    s.number = read(r, 4);
    unsigned flags = read(r, 1);
    s.type = flags & 63;
    unsigned count = read(r, 1) >> 5;
    if (count == 7) {
        --r.position;
        count = read(r, 4) & 0x1fffffff;
        require(count <= 1000000, "JBIG2 segment reference count limit");
        take(r, (size_t(count) + 8) / 8);
    } else
        require(count <= 4, "Invalid JBIG2 segment retention field");
    unsigned bytes = s.number <= 256 ? 1 : s.number <= 65536 ? 2 : 4;
    for (unsigned i = 0; i < count; ++i) {
        auto id = read(r, bytes);
        require(id < s.number, "Forward or cyclic JBIG2 segment reference");
        s.references.push_back(id);
    }
    s.page = read(r, (flags & 64) ? 4 : 1);
    s.length = read(r, 4);
    return s;
}
static void segment_data(Reader& r, Segment& s) {
    if (s.length == UINT32_MAX) {
        require(s.type == 38 && r.data.size() - r.position >= 18, "Unknown-length JBIG2 segment is invalid");
        unsigned flags = r.data[r.position + 17];
        bool mmr = (flags & 1) != 0;
        size_t p = r.position + 18 + (mmr ? 0 : ((flags >> 1) & 3) ? 2 : 8);
        bool found = false;
        for (; p + 5 < r.data.size(); ++p)
            if (r.data[p] == (mmr ? 0 : 255) && r.data[p + 1] == (mmr ? 0 : 172)) {
                require(p + 6 - r.position <= max_stream, "JBIG2 segment size limit");
                s.length = uint32_t(p + 6 - r.position);
                Reader rows{r.data.subspan(p + 2, 4)};
                s.actual_rows = bounded(read(rows, 4));
                require(s.actual_rows >= 0, "Invalid JBIG2 streamed row count");
                found = true;
                break;
            }
        require(found, "Missing JBIG2 unknown-length terminator");
    }
    s.data = take(r, s.length);
}
static void segments(Decoder& d, View data) {
    if (data.empty())
        return;
    Reader r{data};
    bool sequential = true;
    constexpr uint8_t signature[] = {0x97, 'J', 'B', '2', 13, 10, 26, 10};
    if (data.size() >= 9 && std::equal(std::begin(signature), std::end(signature), data.begin())) {
        r.position = 8;
        unsigned flags = read(r, 1);
        require(!(flags & 0xf8), "Unsupported JBIG2 file header flags");
        sequential = (flags & 1) != 0;
        if (!(flags & 2))
            read(r, 4);
    }
    std::vector<Segment> headers;
    if (!sequential) {
        while (r.position < data.size()) {
            require(headers.size() < 100000, "JBIG2 segment count limit");
            headers.push_back(segment_header(r));
            if (headers.back().type == 51)
                break;
        }
        require(!headers.empty() && headers.back().type == 51,
                "JBIG2 random-access header list is incomplete");
    }
    size_t index = 0;
    while (sequential ? r.position < data.size() : index < headers.size()) {
        require(d.segments.size() < 100000, "JBIG2 segment count limit");
        auto s = sequential ? segment_header(r) : std::move(headers[index++]);
        require(!d.segments.contains(s.number), "Duplicate JBIG2 segment number");
        segment_data(r, s);
        decode_segment(d, s);
        unsigned type = s.type;
        d.segments.emplace(s.number, std::move(s));
        if (type == 51)
            break;
    }
}
} // namespace jbig
BilevelImage decode_jbig2(Renderer& renderer, Document& document, View data, View globals, unsigned width,
                          unsigned height) {
    jbig::Decoder decoder{renderer, document, width, height};
    jbig::segments(decoder, globals);
    jbig::segments(decoder, data);
    require(bool(decoder.page), "JBIG2 image has no page bitmap");
    if (unsigned(decoder.page->width) != width) {
        auto cropped = jbig::bitmap(decoder, width, height);
        jbig::combine(*cropped, *decoder.page, 0, 0, 4);
        decoder.page = std::move(cropped);
    }
    return {unsigned(decoder.page->width), unsigned(decoder.page->height), std::move(decoder.page->pixels)};
}
} // namespace pdf
