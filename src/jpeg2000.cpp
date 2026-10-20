#include "image_codecs.h"
#include "parallel.h"
#include <functional>
#include <numeric>
#include <tuple>
#include <chrono>

namespace pdf {
namespace jpx {
struct Reader {
    View data;
    size_t pos = 0;
};
static uint32_t read(Reader& r, unsigned count) {
    require(count <= 4 && r.pos <= r.data.size() && count <= r.data.size() - r.pos,
            "Truncated JPEG 2000 data");
    uint32_t value = 0;
    while (count--)
        value = (value << 8) | r.data[r.pos++];
    return value;
}
static int ceil_div(int n, int d) {
    return n >= 0 ? (n + d - 1) / d : n / d;
}
static int floor_div(int n, int d) {
    return n >= 0 ? n / d : -((-n + d - 1) / d);
}
struct Coding {
    unsigned style = 0, progression = 0, layers = 1, mct = 0, levels = 0, cbx = 6, cby = 6, block_style = 0,
             transform = 1;
    std::vector<std::pair<unsigned, unsigned>> precincts;
};
struct Quant {
    unsigned style = 0, guard = 2;
    std::vector<std::pair<unsigned, unsigned>> steps;
};
struct Component {
    unsigned precision = 8, xstep = 1, ystep = 1;
    bool is_signed = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
struct Header {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, tx0 = 0, ty0 = 0, tw = 0, th = 0;
    unsigned tile_columns = 0, tile_rows = 0;
    std::vector<Component> components;
    Coding coding;
    Quant quant;
    std::unordered_map<unsigned, Coding> coc;
    std::unordered_map<unsigned, Quant> qcc;
};
struct PacketBits {
    View data;
    size_t pos = 0;
    unsigned value = 0, left = 0;
    bool previous_ff = false;
};
static unsigned bit(PacketBits& b) {
    if (!b.left) {
        require(b.pos < b.data.size(), "Truncated JPEG 2000 packet header");
        b.value = b.data[b.pos++];
        b.left = b.previous_ff ? 7 : 8;
        if (b.previous_ff)
            require(!(b.value & 128), "Invalid packet-header stuffed bit");
        b.previous_ff = b.value == 255;
    }
    return (b.value >> (--b.left)) & 1;
}
static uint32_t bits(PacketBits& b, unsigned count) {
    require(count <= 32, "Packet length exceeds 32 bits");
    uint32_t v = 0;
    while (count--)
        v = (v << 1) | bit(b);
    return v;
}
static void align(PacketBits& b) {
    if (b.previous_ff && b.left == 0) {
        require(b.pos < b.data.size(), "Missing packet-header stuffed byte");
        ++b.pos;
    }
    b.left = 0;
    b.previous_ff = false;
}
struct TagNode {
    int value = INT_MAX, low = 0;
};
struct TagTree {
    std::vector<std::vector<TagNode>> levels;
    std::vector<unsigned> widths, heights;
};
static TagTree tag_tree(unsigned w, unsigned h) {
    TagTree t;
    do {
        t.widths.push_back(w);
        t.heights.push_back(h);
        t.levels.emplace_back(size_t(w) * h);
        if (w == 1 && h == 1)
            break;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    } while (w && h);
    return t;
}
static bool tag_decode(TagTree& tree, unsigned x, unsigned y, int threshold, PacketBits& b) {
    int low = 0;
    for (size_t level = tree.levels.size(); level-- > 0;) {
        unsigned xx = x >> level, yy = y >> level;
        require(xx < tree.widths[level] && yy < tree.heights[level], "Invalid JPEG 2000 tag-tree coordinate");
        auto& node = tree.levels[level][size_t(yy) * tree.widths[level] + xx];
        low = std::max(low, node.low);
        while (low < threshold && low < node.value) {
            if (bit(b))
                node.value = low;
            else
                ++low;
        }
        node.low = low;
    }
    return tree.levels[0][size_t(y) * tree.widths[0] + x].value < threshold;
}
struct Block {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned tree_x = 0, tree_y = 0, zero_planes = 0, passes = 0, lblock = 3;
    bool included = false;
    Bytes bytes;
};
struct BandPrecinct {
    std::vector<size_t> blocks;
    TagTree inclusion, zero_planes;
    unsigned columns = 0, rows = 0;
};
struct Band {
    int kind = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned exponent = 0, mantissa = 0;
    std::vector<Block> blocks;
    std::vector<BandPrecinct> precincts;
    std::vector<double> samples;
};
struct Resolution {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, px0 = 0, py0 = 0;
    unsigned ppx = 15, ppy = 15, columns = 0, rows = 0;
    std::vector<Band> bands;
};
struct TileComponent {
    Component image;
    Coding coding;
    Quant quant;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<Resolution> resolutions;
    std::vector<double> samples;
};
struct Packet {
    unsigned layer = 0, resolution = 0, component = 0, precinct = 0;
    int x = 0, y = 0;
};
struct Tile {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Coding coding;
    Quant quant;
    std::unordered_map<unsigned, Coding> coc;
    std::unordered_map<unsigned, Quant> qcc;
    std::vector<TileComponent> components;
    std::vector<Packet> packets;
    size_t next_packet = 0;
    unsigned next_part = 0;
    bool built = false;
};
static Coding coding(Reader& r, const Coding& parent, bool component) {
    Coding c = parent;
    c.style = read(r, 1);
    if (!component) {
        c.progression = read(r, 1);
        c.layers = read(r, 2);
        c.mct = read(r, 1);
    }
    c.levels = read(r, 1);
    c.cbx = (read(r, 1) & 15) + 2;
    c.cby = (read(r, 1) & 15) + 2;
    c.block_style = read(r, 1);
    c.transform = read(r, 1);
    require(c.levels <= 16 && c.cbx <= 10 && c.cby <= 10 && c.cbx + c.cby <= 12 && c.layers > 0 &&
                c.layers <= 1024 && c.progression <= 4 && c.mct <= 1 && c.transform <= 1,
            "Unsupported JPEG 2000 coding parameters");
    require(!(c.block_style & 5), "JPEG 2000 bypass/terminated code-blocks are not supported");
    c.precincts.clear();
    for (unsigned i = 0; i <= c.levels; ++i) {
        unsigned v = (c.style & 1) ? read(r, 1) : 255;
        c.precincts.emplace_back(v & 15, v >> 4);
    }
    return c;
}
static Quant quantization(Reader& r) {
    Quant q;
    unsigned style = read(r, 1);
    q.guard = style >> 5;
    q.style = style & 31;
    require(q.style <= 2, "Invalid JPEG 2000 quantization style");
    while (r.pos < r.data.size()) {
        unsigned v = read(r, q.style ? 2 : 1);
        q.steps.emplace_back(q.style ? v >> 11 : v >> 3, q.style ? v & 2047 : 0);
    }
    require(!q.steps.empty(), "Missing JPEG 2000 quantization steps");
    return q;
}
static void build_tile(Tile& tile, const Header& header) {
    require(!header.components.empty(), "JPEG 2000 image header missing");
    size_t blocks_total = 0;
    for (unsigned ci = 0; ci < header.components.size(); ++ci) {
        TileComponent tc;
        tc.image = header.components[ci];
        tc.coding = tile.coc.contains(ci) ? tile.coc[ci] : tile.coding;
        tc.quant = tile.qcc.contains(ci) ? tile.qcc[ci] : tile.quant;
        tc.x0 = ceil_div(tile.x0, int(tc.image.xstep));
        tc.x1 = ceil_div(tile.x1, int(tc.image.xstep));
        tc.y0 = ceil_div(tile.y0, int(tc.image.ystep));
        tc.y1 = ceil_div(tile.y1, int(tc.image.ystep));
        unsigned quant_index = 0;
        for (unsigned ri = 0; ri <= tc.coding.levels; ++ri) {
            unsigned level = tc.coding.levels - ri;
            int divisor = 1 << level;
            Resolution resolution;
            resolution.x0 = ceil_div(tc.x0, divisor);
            resolution.x1 = ceil_div(tc.x1, divisor);
            resolution.y0 = ceil_div(tc.y0, divisor);
            resolution.y1 = ceil_div(tc.y1, divisor);
            resolution.ppx = tc.coding.precincts[ri].first;
            resolution.ppy = tc.coding.precincts[ri].second;
            require(ri == 0 || (resolution.ppx > 0 && resolution.ppy > 0),
                    "JPEG 2000 one-sample high-frequency precincts are not supported");
            int pw = 1 << resolution.ppx, ph = 1 << resolution.ppy;
            resolution.px0 = floor_div(resolution.x0, pw);
            resolution.py0 = floor_div(resolution.y0, ph);
            resolution.columns = unsigned(std::max(0, ceil_div(resolution.x1, pw) - resolution.px0));
            resolution.rows = unsigned(std::max(0, ceil_div(resolution.y1, ph) - resolution.py0));
            require(uint64_t(resolution.columns) * resolution.rows <= 1000000,
                    "JPEG 2000 precinct count limit exceeded");
            for (int kind = ri ? 1 : 0; kind <= (ri ? 3 : 0); ++kind) {
                Band band;
                band.kind = kind;
                if (!ri) {
                    band.x0 = resolution.x0;
                    band.x1 = resolution.x1;
                    band.y0 = resolution.y0;
                    band.y1 = resolution.y1;
                } else {
                    int xd = kind & 1, yd = kind >> 1;
                    band.x0 = ceil_div(tc.x0 - divisor * xd, divisor * 2);
                    band.x1 = ceil_div(tc.x1 - divisor * xd, divisor * 2);
                    band.y0 = ceil_div(tc.y0 - divisor * yd, divisor * 2);
                    band.y1 = ceil_div(tc.y1 - divisor * yd, divisor * 2);
                }
                if (tc.quant.style == 1) {
                    band.exponent = tc.quant.steps[0].first;
                    if (ri)
                        band.exponent = unsigned(std::max(0, int(band.exponent) - int(ri) + 1));
                    band.mantissa = tc.quant.steps[0].second;
                } else {
                    require(quant_index < tc.quant.steps.size(), "Missing subband quantization step");
                    band.exponent = tc.quant.steps[quant_index].first;
                    band.mantissa = tc.quant.steps[quant_index].second;
                }
                ++quant_index;
                unsigned precinct_x_bits = resolution.ppx - (ri ? 1 : 0),
                         precinct_y_bits = resolution.ppy - (ri ? 1 : 0);
                int bpw = 1 << precinct_x_bits, bph = 1 << precinct_y_bits;
                int cbw = 1 << std::min(tc.coding.cbx, precinct_x_bits),
                    cbh = 1 << std::min(tc.coding.cby, precinct_y_bits);
                band.precincts.resize(size_t(resolution.columns) * resolution.rows);
                for (unsigned py = 0; py < resolution.rows; ++py)
                    for (unsigned px = 0; px < resolution.columns; ++px) {
                        int bx0 = std::max(band.x0, (resolution.px0 + int(px)) * bpw),
                            by0 = std::max(band.y0, (resolution.py0 + int(py)) * bph),
                            bx1 = std::min(band.x1, (resolution.px0 + int(px) + 1) * bpw),
                            by1 = std::min(band.y1, (resolution.py0 + int(py) + 1) * bph);
                        auto& precinct = band.precincts[size_t(py) * resolution.columns + px];
                        if (bx1 <= bx0 || by1 <= by0)
                            continue;
                        int cx0 = floor_div(bx0, cbw), cy0 = floor_div(by0, cbh), cx1 = ceil_div(bx1, cbw),
                            cy1 = ceil_div(by1, cbh);
                        precinct.columns = unsigned(cx1 - cx0);
                        precinct.rows = unsigned(cy1 - cy0);
                        precinct.inclusion = tag_tree(precinct.columns, precinct.rows);
                        precinct.zero_planes = tag_tree(precinct.columns, precinct.rows);
                        for (int cy = cy0; cy < cy1; ++cy)
                            for (int cx = cx0; cx < cx1; ++cx) {
                                require(++blocks_total <= 1000000, "JPEG 2000 code-block limit exceeded");
                                Block block;
                                block.x0 = std::max(bx0, cx * cbw);
                                block.x1 = std::min(bx1, (cx + 1) * cbw);
                                block.y0 = std::max(by0, cy * cbh);
                                block.y1 = std::min(by1, (cy + 1) * cbh);
                                block.tree_x = unsigned(cx - cx0);
                                block.tree_y = unsigned(cy - cy0);
                                precinct.blocks.push_back(band.blocks.size());
                                band.blocks.push_back(std::move(block));
                            }
                    }
                resolution.bands.push_back(std::move(band));
            }
            tc.resolutions.push_back(std::move(resolution));
        }
        tile.components.push_back(std::move(tc));
    }
    for (unsigned ci = 0; ci < tile.components.size(); ++ci) {
        auto& tc = tile.components[ci];
        for (unsigned ri = 0; ri < tc.resolutions.size(); ++ri) {
            auto& res = tc.resolutions[ri];
            unsigned scale = 1u << (tc.coding.levels - ri);
            for (unsigned py = 0; py < res.rows; ++py)
                for (unsigned px = 0; px < res.columns; ++px)
                    for (unsigned layer = 0; layer < tile.coding.layers; ++layer) {
                        require(tile.packets.size() < 2'000'000, "JPEG 2000 packet limit exceeded");
                        int x = std::max(tile.x0, (res.px0 + int(px)) * (1 << res.ppx) * int(scale) *
                                                      int(tc.image.xstep)),
                            y = std::max(tile.y0, (res.py0 + int(py)) * (1 << res.ppy) * int(scale) *
                                                      int(tc.image.ystep));
                        tile.packets.push_back({layer, ri, ci, py * res.columns + px, x, y});
                    }
        }
    }
    unsigned progression = tile.coding.progression;
    std::sort(tile.packets.begin(), tile.packets.end(), [&](const Packet& a, const Packet& b) {
        if (progression == 0)
            return std::tie(a.layer, a.resolution, a.component, a.precinct) <
                   std::tie(b.layer, b.resolution, b.component, b.precinct);
        if (progression == 1)
            return std::tie(a.resolution, a.layer, a.component, a.precinct) <
                   std::tie(b.resolution, b.layer, b.component, b.precinct);
        if (progression == 2)
            return std::tie(a.resolution, a.y, a.x, a.component, a.layer) <
                   std::tie(b.resolution, b.y, b.x, b.component, b.layer);
        if (progression == 3)
            return std::tie(a.y, a.x, a.component, a.resolution, a.layer) <
                   std::tie(b.y, b.x, b.component, b.resolution, b.layer);
        return std::tie(a.component, a.y, a.x, a.resolution, a.layer) <
               std::tie(b.component, b.y, b.x, b.resolution, b.layer);
    });
    tile.built = true;
}
static unsigned coding_passes(PacketBits& b) {
    if (!bit(b))
        return 1;
    if (!bit(b))
        return 2;
    unsigned n = bits(b, 2);
    if (n < 3)
        return n + 3;
    n = bits(b, 5);
    return n < 31 ? n + 6 : bits(b, 7) + 37;
}
static void packets(Document& document, Tile& tile, View data) {
    PacketBits reader{data};
    while (reader.pos < data.size() && tile.next_packet < tile.packets.size()) {
        check_cancel(document);
        if (reader.pos + 1 < data.size() && data[reader.pos] == 255 && data[reader.pos + 1] == 0x91) {
            require(data.size() - reader.pos >= 6, "Truncated SOP marker");
            reader.pos += 6;
        }
        auto packet = tile.packets[tile.next_packet++];
        auto& component = tile.components[packet.component];
        auto& resolution = component.resolutions[packet.resolution];
        std::vector<std::pair<Block*, size_t>> contributions;
        if (bit(reader))
            for (auto& band : resolution.bands) {
                auto& precinct = band.precincts[packet.precinct];
                for (auto index : precinct.blocks) {
                    auto& block = band.blocks[index];
                    bool included = block.included ? bit(reader) != 0
                                                   : tag_decode(precinct.inclusion, block.tree_x,
                                                                block.tree_y, int(packet.layer + 1), reader);
                    if (!included)
                        continue;
                    if (!block.included) {
                        int zero = 0;
                        while (
                            !tag_decode(precinct.zero_planes, block.tree_x, block.tree_y, zero + 1, reader))
                            require(++zero < 32, "JPEG 2000 bit-plane limit exceeded");
                        block.zero_planes = unsigned(zero);
                        block.included = true;
                    }
                    unsigned passes = coding_passes(reader);
                    while (bit(reader))
                        require(++block.lblock <= 31, "JPEG 2000 code-block length overflow");
                    unsigned log = 0;
                    for (unsigned n = passes; n > 1; n >>= 1)
                        ++log;
                    size_t length = bits(reader, block.lblock + log);
                    require(block.passes + passes <= 96, "JPEG 2000 coding-pass limit exceeded");
                    block.passes += passes;
                    contributions.emplace_back(&block, length);
                }
            }
        align(reader);
        if (reader.pos + 1 < data.size() && data[reader.pos] == 255 && data[reader.pos + 1] == 0x92)
            reader.pos += 2;
        for (auto [block, length] : contributions) {
            require(length <= data.size() - reader.pos && length <= max_stream - block->bytes.size(),
                    "Truncated JPEG 2000 packet body");
            block->bytes.insert(block->bytes.end(), data.begin() + reader.pos,
                                data.begin() + reader.pos + length);
            reader.pos += length;
        }
    }
    require(reader.pos == data.size(), "JPEG 2000 tile part has unexpected trailing data");
}
struct Coefficient {
    int32_t magnitude = 0;
    uint32_t flags = 0;
};
// Neighbour significance changes only when a coefficient first becomes significant.
// Keep it beside the coefficient instead of probing eight bounded neighbours for
// every bit plane. A one-cell halo makes those updates safe at block edges.
enum CoefficientFlags : uint32_t {
    west = 1,
    east = 2,
    north = 4,
    south = 8,
    northwest = 16,
    northeast = 32,
    southwest = 64,
    southeast = 128,
    negative_west = 256,
    negative_east = 512,
    negative_north = 1024,
    negative_south = 2048,
    significant_flag = 4096,
    visited_flag = 8192,
    fresh_flag = 16384,
    refined_flag = 32768,
    negative_flag = 65536
};
static constexpr auto significance_contexts = [] {
    std::array<std::array<uint8_t, 256>, 4> tables{};
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned flags = 0; flags < 256; ++flags) {
            int h = bool(flags & west) + bool(flags & east), v = bool(flags & north) + bool(flags & south),
                diagonal = bool(flags & northwest) + bool(flags & northeast) + bool(flags & southwest) +
                           bool(flags & southeast);
            if (kind == 1)
                std::swap(h, v);
            int hv = h + v;
            int context = kind == 3 ? (diagonal >= 3   ? 8
                                       : diagonal == 2 ? (hv ? 7 : 6)
                                       : diagonal == 1 ? (hv >= 2   ? 5
                                                          : hv == 1 ? 4
                                                                    : 3)
                                       : hv >= 2       ? 2
                                       : hv == 1       ? 1
                                                       : 0)
                                    : (h == 2          ? 8
                                       : h == 1        ? (v          ? 7
                                                          : diagonal ? 6
                                                                     : 5)
                                       : v == 2        ? 4
                                       : v == 1        ? 3
                                       : diagonal >= 2 ? 2
                                       : diagonal == 1 ? 1
                                                       : 0);
            tables[kind][flags] = uint8_t(context);
        }
    return tables;
}();
static constexpr auto sign_contexts = [] {
    std::array<uint8_t, 256> table{};
    for (unsigned flags = 0; flags < 256; ++flags) {
        auto signed_neighbor = [&](unsigned direction) {
            return flags & direction ? (flags & (direction << 4) ? -1 : 1) : 0;
        };
        int h = std::clamp(signed_neighbor(west) + signed_neighbor(east), -1, 1),
            v = std::clamp(signed_neighbor(north) + signed_neighbor(south), -1, 1);
        bool prediction = h < 0 || (h == 0 && v < 0);
        if (h < 0) {
            h = -h;
            v = -v;
        } else if (h == 0 && v < 0)
            v = -v;
        int context = h == 0 ? (v == 0 ? 9 : 10) : v < 0 ? 11 : v == 0 ? 12 : 13;
        table[flags] = uint8_t((context << 1) | unsigned(prediction));
    }
    return table;
}();
static void decode_block(const TileComponent& component, Band& band, Block& block) {
    if (!block.passes)
        return;
    int width = block.x1 - block.x0, height = block.y1 - block.y0;
    int stride = width + 2;
    std::vector<Coefficient> samples(size_t(stride) * (height + 2));
    MQDecoder mq;
    mq_init(mq, block.bytes);
    std::array<uint8_t, 19> contexts{};
    auto reset = [&] {
        contexts.fill(0);
        contexts[0] = 4 << 1;
        contexts[17] = 3 << 1;
        contexts[18] = 46 << 1;
    };
    reset();
    int plane = int(component.quant.guard) + int(band.exponent) - 2 - int(block.zero_planes);
    require(plane >= 0 && plane <= 28, "Invalid JPEG 2000 magnitude bit plane");
    bool causal = (component.coding.block_style & 8) != 0;
    const auto& significance = significance_contexts[size_t(band.kind)];
    auto flags_for = [&](const Coefficient& s, int y) {
        return causal && (y & 3) == 3 ? s.flags & ~(south | southwest | southeast | negative_south) : s.flags;
    };
    auto significant = [&](Coefficient* s, uint32_t flags) {
        unsigned code = sign_contexts[(flags & 15) | ((flags >> 4) & 240)];
        unsigned negative = mq_bit(mq, contexts[code >> 1]) ^ (code & 1);
        s->magnitude = 3 * (1 << plane);
        s->flags |= significant_flag | fresh_flag | (negative ? negative_flag : 0);
        s[-1].flags |= east | (negative ? negative_east : 0);
        s[1].flags |= west | (negative ? negative_west : 0);
        s[-stride].flags |= south | (negative ? negative_south : 0);
        s[stride].flags |= north | (negative ? negative_north : 0);
        s[-stride - 1].flags |= southeast;
        s[-stride + 1].flags |= southwest;
        s[stride - 1].flags |= northeast;
        s[stride + 1].flags |= northwest;
    };
    auto decode_pass = [&]<unsigned kind>() {
        for (int stripe = 0; stripe < height; stripe += 4)
            for (int x = 0; x < width; ++x) {
                int end = std::min(stripe + 4, height), begin = stripe;
                if (kind == 2 && end - stripe == 4) {
                    bool run = true;
                    for (int y = stripe; y < end; ++y) {
                        auto& s = samples[size_t(y + 1) * stride + x + 1];
                        if (flags_for(s, y) & (significant_flag | visited_flag | 255)) {
                            run = false;
                            break;
                        }
                    }
                    if (run) {
                        if (!mq_bit(mq, contexts[17]))
                            continue;
                        begin += int((mq_bit(mq, contexts[18]) << 1) | mq_bit(mq, contexts[18]));
                        auto* s = &samples[size_t(begin + 1) * stride + x + 1];
                        significant(s, flags_for(*s, begin));
                        ++begin;
                    }
                }
                for (int y = begin; y < end; ++y) {
                    auto& s = samples[size_t(y + 1) * stride + x + 1];
                    uint32_t flags = flags_for(s, y);
                    if (kind == 0) {
                        if (!(flags & significant_flag) && (flags & 255)) {
                            s.flags |= visited_flag;
                            if (mq_bit(mq, contexts[significance[flags & 255]]))
                                significant(&s, flags);
                        }
                    } else if (kind == 1) {
                        if ((flags & (significant_flag | fresh_flag)) == significant_flag) {
                            unsigned context = flags & refined_flag ? 16 : flags & 255 ? 15 : 14;
                            int value = mq_bit(mq, contexts[context]) ? 1 : -1;
                            s.magnitude += value * (1 << plane);
                            s.flags |= refined_flag;
                        }
                    } else if (!(flags & (significant_flag | visited_flag)) &&
                               mq_bit(mq, contexts[significance[flags & 255]]))
                        significant(&s, flags);
                }
            }
    };
    for (unsigned pass = 0; pass < block.passes; ++pass) {
        require(plane >= 0, "Too many JPEG 2000 coding passes");
        unsigned kind = (pass + 2) % 3;
        if (kind == 0)
            decode_pass.template operator()<0>();
        else if (kind == 1)
            decode_pass.template operator()<1>();
        else
            decode_pass.template operator()<2>();
        if (kind == 2) {
            if (component.coding.block_style & 32) {
                unsigned symbol = 0;
                for (int i = 0; i < 4; ++i)
                    symbol = (symbol << 1) | mq_bit(mq, contexts[18]);
                require(symbol == 10, "Invalid JPEG 2000 segmentation symbol");
            }
            for (auto& sample : samples)
                sample.flags &= ~(visited_flag | fresh_flag);
            --plane;
        }
        if (component.coding.block_style & 2)
            reset();
    }
    double delta = 1;
    if (component.quant.style) {
        int gain = band.kind == 3 ? 2 : band.kind ? 1 : 0;
        delta = std::ldexp(1.0, int(component.image.precision) + gain - int(band.exponent)) *
                (1 + band.mantissa / 2048.0);
    }
    int band_width = band.x1 - band.x0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto& s = samples[size_t(y + 1) * stride + x + 1];
            double value = component.coding.transform ? double(s.magnitude / 2) : s.magnitude / 2.0;
            if (s.flags & negative_flag)
                value = -value;
            band.samples[size_t(block.y0 - band.y0 + y) * band_width + block.x0 - band.x0 + x] =
                value * delta;
        }
}
static double floor_coefficient(double value) {
    // SSE2 has truncating 64-bit conversion; avoid a CRT floor call at every
    // reversible lifting step. Outside this interval doubles are already integral.
    if (value > -0x1p52 && value < 0x1p52) {
        auto truncated = int64_t(value);
        return double(truncated - (double(truncated) > value));
    }
    return value;
}
static void inverse_line(std::span<double> data, int origin, bool reversible) {
    size_t n = data.size();
    if (n <= 1) {
        if (n == 1 && (origin & 1))
            data[0] *= 0.5;
        return;
    }
    auto neighbor = [&](int index) {
        if (index < 0)
            index = 1;
        if (index >= int(n))
            index = int(n) - 2;
        return data[size_t(index)];
    };
    int low = (origin & 1) ? 1 : 0, high = 1 - low;
    if (reversible) {
        for (int i = low; i < int(n); i += 2)
            data[size_t(i)] -= floor_coefficient((neighbor(i - 1) + neighbor(i + 1) + 2) / 4);
        for (int i = high; i < int(n); i += 2)
            data[size_t(i)] += floor_coefficient((neighbor(i - 1) + neighbor(i + 1)) / 2);
    } else {
        constexpr double scale = 1.230174104914001;
        for (int i = low; i < int(n); i += 2)
            data[size_t(i)] *= scale;
        for (int i = high; i < int(n); i += 2)
            data[size_t(i)] /= scale;
        for (auto [parity, factor] :
             {std::pair{low, 0.443506852043971}, std::pair{high, 0.882911075530934},
              std::pair{low, -0.052980118572961}, std::pair{high, -1.586134342059924}})
            for (int i = parity; i < int(n); i += 2)
                data[size_t(i)] -= factor * (neighbor(i - 1) + neighbor(i + 1));
    }
}
static void reconstruct(Document& doc, TileComponent& tc, JpxOptions options) {
    auto* profile = options.profile;
    auto begin = std::chrono::steady_clock::now();
    std::vector<std::pair<Band*, Block*>> jobs;
    size_t levels = tc.resolutions.size() - options.reduction;
    for (auto& resolution : std::span(tc.resolutions).first(levels))
        for (auto& band : resolution.bands) {
            uint64_t count = uint64_t(std::max(0, band.x1 - band.x0)) * std::max(0, band.y1 - band.y0);
            require(count <= max_stream / sizeof(double), "JPEG 2000 coefficient allocation limit");
            band.samples.resize(size_t(count));
            for (auto& block : band.blocks)
                jobs.emplace_back(&band, &block);
        }
    parallel_for(jobs.size(), 8, options.workers, [&](size_t i) {
        check_cancel(doc);
        auto [band, block] = jobs[i];
        decode_block(tc, *band, *block);
        Bytes{}.swap(block->bytes);
    });
    auto wavelet_begin = std::chrono::steady_clock::now();
    if (profile)
        profile->blocks_ms += std::chrono::duration<double, std::milli>(wavelet_begin - begin).count();
    auto& first = tc.resolutions[0];
    tc.samples = std::move(first.bands[0].samples);
    int previous_x0 = first.x0, previous_y0 = first.y0, previous_width = first.x1 - first.x0,
        previous_height = first.y1 - first.y0;
    for (size_t ri = 1; ri < levels; ++ri) {
        check_cancel(doc);
        auto& res = tc.resolutions[ri];
        int width = res.x1 - res.x0, height = res.y1 - res.y0;
        require(uint64_t(width) * height <= max_stream / sizeof(double), "JPEG 2000 reconstruction limit");
        std::vector<double> grid(size_t(width) * height);
        for (int y = 0; y < previous_height; ++y)
            for (int x = 0; x < previous_width; ++x) {
                int xx = (x + previous_x0) * 2 - res.x0, yy = (y + previous_y0) * 2 - res.y0;
                require(xx >= 0 && yy >= 0 && xx < width && yy < height, "Invalid wavelet low-pass geometry");
                grid[size_t(yy) * width + xx] = tc.samples[size_t(y) * previous_width + x];
            }
        for (auto& band : res.bands) {
            int bw = band.x1 - band.x0, bh = band.y1 - band.y0, dx = band.kind & 1, dy = band.kind >> 1;
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    int xx = (x + band.x0) * 2 + dx - res.x0, yy = (y + band.y0) * 2 + dy - res.y0;
                    require(xx >= 0 && yy >= 0 && xx < width && yy < height,
                            "Invalid wavelet high-pass geometry");
                    grid[size_t(yy) * width + xx] = band.samples[size_t(y) * bw + x];
                }
            std::vector<double>{}.swap(band.samples);
        }
        // The reversible lifting steps round to integers, so the order of the
        // two inverse axes matters: horizontal synthesis precedes vertical.
        for (int y = 0; y < height; ++y) {
            inverse_line(std::span(grid).subspan(size_t(y) * width, width), res.x0, tc.coding.transform != 0);
        }
        // Gather columns in small strips so each row's cache line is reused,
        // instead of walking the entire image with a page-sized stride per column.
        constexpr int strip = 16;
        std::vector<double> columns(size_t(std::min(strip, width)) * height);
        for (int x = 0; x < width; x += strip) {
            int count = std::min(strip, width - x);
            for (int y = 0; y < height; ++y)
                for (int dx = 0; dx < count; ++dx)
                    columns[size_t(dx) * height + y] = grid[size_t(y) * width + x + dx];
            for (int dx = 0; dx < count; ++dx)
                inverse_line(std::span(columns).subspan(size_t(dx) * height, height), res.y0,
                             tc.coding.transform != 0);
            for (int y = 0; y < height; ++y)
                for (int dx = 0; dx < count; ++dx)
                    grid[size_t(y) * width + x + dx] = columns[size_t(dx) * height + y];
        }
        tc.samples = std::move(grid);
        previous_width = width;
        previous_height = height;
        previous_x0 = res.x0;
        previous_y0 = res.y0;
    }
    if (profile)
        profile->wavelet_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wavelet_begin)
                .count();
    const auto& final = tc.resolutions[levels - 1];
    tc.x0 = final.x0;
    tc.y0 = final.y0;
    tc.x1 = final.x1;
    tc.y1 = final.y1;
}
static JpxImage codestream(Document& document, View bytes, JpxOptions options) {
    auto* profile = options.profile;
    auto begin = std::chrono::steady_clock::now();
    Reader r{bytes};
    require(read(r, 2) == 0xff4f, "Missing JPEG 2000 codestream marker");
    Header header;
    std::vector<Tile> tiles;
    Tile* current = nullptr;
    size_t tile_end = 0;
    bool finished = false;
    while (r.pos < bytes.size()) {
        check_cancel(document);
        size_t marker_position = r.pos;
        unsigned marker = read(r, 2);
        require((marker & 0xff00) == 0xff00, "Invalid JPEG 2000 marker");
        if (marker == 0xffd9) {
            finished = true;
            break;
        }
        if (marker == 0xff93) {
            require(current && tile_end >= r.pos && tile_end <= bytes.size(),
                    "Invalid JPEG 2000 tile-part length");
            if (!current->built)
                build_tile(*current, header);
            packets(document, *current, bytes.subspan(r.pos, tile_end - r.pos));
            r.pos = tile_end;
            current = nullptr;
            continue;
        }
        unsigned length = read(r, 2);
        require(length >= 2 && length - 2 <= bytes.size() - r.pos, "Invalid JPEG 2000 marker length");
        Reader segment{bytes.subspan(r.pos, length - 2)};
        r.pos += length - 2;
        if (marker == 0xff51) {
            require(header.components.empty() && !current, "Duplicate JPEG 2000 size marker");
            read(segment, 2);
            header.x1 = int(read(segment, 4));
            header.y1 = int(read(segment, 4));
            header.x0 = int(read(segment, 4));
            header.y0 = int(read(segment, 4));
            header.tw = int(read(segment, 4));
            header.th = int(read(segment, 4));
            header.tx0 = int(read(segment, 4));
            header.ty0 = int(read(segment, 4));
            unsigned count = read(segment, 2);
            require(header.x0 >= 0 && header.y0 >= 0 && header.x1 > header.x0 && header.y1 > header.y0 &&
                        header.x1 <= 1'000'000 && header.y1 <= 1'000'000 && header.tw > 0 && header.th > 0 &&
                        header.tw <= 1'000'000 && header.th <= 1'000'000 && header.tx0 >= 0 &&
                        header.ty0 >= 0 && header.tx0 <= header.x0 && header.ty0 <= header.y0 &&
                        header.tx0 + header.tw > header.x0 && header.ty0 + header.th > header.y0 &&
                        count >= 1 && count <= 16,
                    "Invalid JPEG 2000 image dimensions");
            require(uint64_t(header.x1 - header.x0) * (header.y1 - header.y0) * count <=
                        max_stream / sizeof(double),
                    "JPEG 2000 image allocation limit");
            header.tile_columns = unsigned(ceil_div(header.x1 - header.tx0, header.tw));
            header.tile_rows = unsigned(ceil_div(header.y1 - header.ty0, header.th));
            require(uint64_t(header.tile_columns) * header.tile_rows <= 65535, "JPEG 2000 tile limit");
            for (unsigned i = 0; i < count; ++i) {
                Component c;
                unsigned ssiz = read(segment, 1);
                c.precision = (ssiz & 127) + 1;
                c.is_signed = (ssiz & 128) != 0;
                c.xstep = read(segment, 1);
                c.ystep = read(segment, 1);
                require(c.precision <= 16 && c.xstep && c.ystep, "Unsupported JPEG 2000 component geometry");
                c.x0 = ceil_div(header.x0, int(c.xstep));
                c.x1 = ceil_div(header.x1, int(c.xstep));
                c.y0 = ceil_div(header.y0, int(c.ystep));
                c.y1 = ceil_div(header.y1, int(c.ystep));
                header.components.push_back(c);
            }
            tiles.resize(size_t(header.tile_columns) * header.tile_rows);
        } else if (marker == 0xff52) {
            auto& target = current ? current->coding : header.coding;
            target = coding(segment, target, false);
        } else if (marker == 0xff53) {
            unsigned component = read(segment, header.components.size() < 257 ? 1 : 2);
            require(component < header.components.size(), "Invalid JPEG 2000 COC component");
            auto& target = current ? current->coc : header.coc;
            target[component] = coding(segment, current ? current->coding : header.coding, true);
        } else if (marker == 0xff5c) {
            (current ? current->quant : header.quant) = quantization(segment);
        } else if (marker == 0xff5d) {
            unsigned component = read(segment, header.components.size() < 257 ? 1 : 2);
            require(component < header.components.size(), "Invalid JPEG 2000 QCC component");
            (current ? current->qcc : header.qcc)[component] = quantization(segment);
        } else if (marker == 0xff90) {
            unsigned index = read(segment, 2), size = read(segment, 4), part = read(segment, 1);
            read(segment, 1);
            require(!current && index < tiles.size(), "Invalid JPEG 2000 tile index");
            current = &tiles[index];
            require(part == current->next_part++, "Missing or out-of-order JPEG 2000 tile part");
            tile_end = size ? marker_position + size : bytes.size() - 2;
            require(tile_end <= bytes.size() && tile_end > r.pos, "Invalid JPEG 2000 tile-part boundary");
            if (part == 0) {
                require(!current->built, "Duplicate first JPEG 2000 tile part");
                int x = int(index % header.tile_columns), y = int(index / header.tile_columns);
                current->x0 = std::max(header.x0, header.tx0 + x * header.tw);
                current->y0 = std::max(header.y0, header.ty0 + y * header.th);
                current->x1 = std::min(header.x1, header.tx0 + (x + 1) * header.tw);
                current->y1 = std::min(header.y1, header.ty0 + (y + 1) * header.th);
                current->coding = header.coding;
                current->quant = header.quant;
                current->coc = header.coc;
                current->qcc = header.qcc;
            }
        } else if (marker == 0xff5e || marker == 0xff5f || marker == 0xff60 || marker == 0xff61)
            fail("JPEG 2000 ROI/progression changes or packed packet headers are not supported");
        else if (marker == 0xff50)
            fail("High-throughput JPEG 2000 is not supported");
    }
    require(finished && !tiles.empty(), "Incomplete JPEG 2000 codestream");
    if (profile)
        profile->packets_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    JpxImage image;
    // All components/tiles must use the same reduction. Packet headers are still
    // validated in full, but high-frequency codeblocks need not be reconstructed.
    options.reduction = std::min(options.reduction, 16u);
    for (const auto& tile : tiles) {
        require(tile.built, "JPEG 2000 tile data missing");
        require(tile.next_packet == tile.packets.size(), "Incomplete JPEG 2000 tile packets");
        for (const auto& component : tile.components)
            options.reduction = std::min(options.reduction, component.coding.levels);
    }
    // Very small edge tiles or subsampled components can become empty at a
    // requested level. Retain another level rather than dropping their samples.
    while (options.reduction) {
        int candidate_divisor = 1 << options.reduction;
        bool usable = ceil_div(header.x1, candidate_divisor) > ceil_div(header.x0, candidate_divisor) &&
                      ceil_div(header.y1, candidate_divisor) > ceil_div(header.y0, candidate_divisor);
        for (const auto& tile : tiles) {
            if (ceil_div(tile.x1, candidate_divisor) == ceil_div(tile.x0, candidate_divisor) ||
                ceil_div(tile.y1, candidate_divisor) == ceil_div(tile.y0, candidate_divisor))
                continue;
            for (const auto& component : tile.components) {
                const auto& res = component.resolutions[component.resolutions.size() - 1 - options.reduction];
                usable = usable && res.x1 > res.x0 && res.y1 > res.y0;
            }
        }
        if (usable)
            break;
        --options.reduction;
    }
    int divisor = 1 << options.reduction;
    int output_x0 = ceil_div(header.x0, divisor), output_y0 = ceil_div(header.y0, divisor);
    image.source_width = unsigned(header.x1 - header.x0);
    image.source_height = unsigned(header.y1 - header.y0);
    image.reduction = options.reduction;
    image.width = unsigned(ceil_div(header.x1, divisor) - output_x0);
    image.height = unsigned(ceil_div(header.y1, divisor) - output_y0);
    require(image.width && image.height, "Empty reduced JPEG 2000 image");
    image.components = unsigned(header.components.size());
    image.samples.resize(size_t(image.width) * image.height * image.components);
    std::vector<JpxProfile> tile_profiles(profile ? tiles.size() : 0);
    parallel_for(tiles.size(), 1, options.workers, [&](size_t index) {
        auto& tile = tiles[index];
        JpxOptions tile_options = options;
        // Prefer whole tiles: small artbook tiles have too few codeblocks at
        // display resolution to use a core efficiently. Never nest pool jobs.
        if (tiles.size() > 1)
            tile_options.workers = 1;
        tile_options.profile = profile ? &tile_profiles[index] : nullptr;
        require(tile.built, "JPEG 2000 tile data missing");
        require(tile.next_packet == tile.packets.size(), "Incomplete JPEG 2000 tile packets");
        int tile_x0 = ceil_div(tile.x0, divisor), tile_y0 = ceil_div(tile.y0, divisor),
            tile_x1 = ceil_div(tile.x1, divisor), tile_y1 = ceil_div(tile.y1, divisor);
        if (tile_x0 == tile_x1 || tile_y0 == tile_y1)
            return;
        for (auto& c : tile.components) {
            reconstruct(document, c, tile_options);
            require(c.x1 > c.x0 && c.y1 > c.y0, "Empty reduced JPEG 2000 component");
        }
        auto output_begin = std::chrono::steady_clock::now();
        parallel_for(size_t(tile_y1 - tile_y0), 32, tile_options.workers, [&](size_t row) {
            int y = tile_y0 + int(row);
            if ((y & 127) == 0)
                check_cancel(document);
            for (int x = tile_x0; x < tile_x1; ++x) {
                std::array<double, 16> values{};
                for (size_t i = 0; i < tile.components.size(); ++i) {
                    auto& c = tile.components[i];
                    int xx = std::clamp(floor_div(x, int(c.image.xstep)), c.x0, c.x1 - 1) - c.x0,
                        yy = std::clamp(floor_div(y, int(c.image.ystep)), c.y0, c.y1 - 1) - c.y0;
                    values[i] = c.samples[size_t(yy) * (c.x1 - c.x0) + xx];
                }
                if (tile.coding.mct && image.components >= 3) {
                    double yy = values[0], cb = values[1], cr = values[2];
                    if (tile.components[0].coding.transform) {
                        double g = yy - floor_coefficient((cb + cr) / 4);
                        values[0] = g + cr;
                        values[1] = g;
                        values[2] = g + cb;
                    } else {
                        values[0] = yy + 1.402 * cr;
                        values[1] = yy - 0.34413 * cb - 0.71414 * cr;
                        values[2] = yy + 1.772 * cb;
                    }
                }
                size_t index = (size_t(y - output_y0) * image.width + x - output_x0) * image.components;
                for (unsigned i = 0; i < image.components; ++i) {
                    auto& component = header.components[i];
                    // Unsigned components need inverse DC shifting; signed components
                    // need the same bias to normalize their display range to 0..255.
                    double value = values[i] + (1u << (component.precision - 1));
                    double maximum = (1u << component.precision) - 1;
                    image.samples[index + i] = uint8_t(std::clamp(value * 255 / maximum + 0.5, 0.0, 255.0));
                }
            }
        });
        for (auto& c : tile.components) {
            std::vector<double>{}.swap(c.samples);
            c.resolutions.clear();
        }
        if (tile_options.profile)
            tile_options.profile->output_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - output_begin)
                    .count();
    });
    if (profile)
        for (const auto& tile_profile : tile_profiles) {
            profile->blocks_ms += tile_profile.blocks_ms;
            profile->wavelet_ms += tile_profile.wavelet_ms;
            profile->output_ms += tile_profile.output_ms;
        }
    return image;
}
} // namespace jpx

JpxImage decode_jpx(Document& doc, View data, JpxOptions options) {
    if (data.size() >= 2 && data[0] == 255 && data[1] == 0x4f)
        return jpx::codestream(doc, data, options);
    View codestream;
    int alpha = -1;
    bool premultiplied = false;
    unsigned enumerated_color = 0;
    std::function<void(View, int)> inspect = [&](View input, int depth) {
        require(depth < 8, "JPEG 2000 box nesting limit");
        jpx::Reader r{input};
        while (r.pos < input.size()) {
            size_t start = r.pos;
            uint64_t length = jpx::read(r, 4);
            uint32_t type = jpx::read(r, 4);
            if (length == 1) {
                uint64_t high = jpx::read(r, 4), low = jpx::read(r, 4);
                length = (high << 32) | low;
            } else if (length == 0)
                length = input.size() - start;
            require(length >= r.pos - start && length <= input.size() - start,
                    "Invalid JPEG 2000 box length");
            View body = input.subspan(r.pos, size_t(length) - (r.pos - start));
            if (type == 0x6a703263)
                codestream = body;
            else if (type == 0x6a703268)
                inspect(body, depth + 1);
            else if (type == 0x63646566) {
                jpx::Reader definition{body};
                unsigned count = jpx::read(definition, 2);
                for (unsigned i = 0; i < count; ++i) {
                    unsigned channel = jpx::read(definition, 2), kind = jpx::read(definition, 2);
                    jpx::read(definition, 2);
                    if (kind == 1 || kind == 2) {
                        alpha = int(channel);
                        premultiplied = kind == 2;
                    }
                }
            } else if (type == 0x636f6c72 && body.size() >= 7 && body[0] == 1) {
                jpx::Reader color{body, 3};
                enumerated_color = jpx::read(color, 4);
            } else if (type == 0x70636c72 || type == 0x636d6170)
                fail("JPEG 2000 palette/channel mappings are not supported");
            r.pos = start + size_t(length);
        }
    };
    inspect(data, 0);
    require(!codestream.empty(), "No JPEG 2000 codestream box");
    auto image = jpx::codestream(doc, codestream, options);
    require(alpha < 0 || unsigned(alpha) < image.components, "Invalid JPEG 2000 alpha channel");
    image.alpha = alpha;
    image.premultiplied = premultiplied;
    if (enumerated_color == 18 && image.components >= 3)
        for (size_t i = 0; i < image.samples.size(); i += image.components) {
            double y = image.samples[i], cb = image.samples[i + 1] - 128.0, cr = image.samples[i + 2] - 128.0;
            image.samples[i] = uint8_t(std::clamp(y + 1.402 * cr, 0.0, 255.0));
            image.samples[i + 1] = uint8_t(std::clamp(y - 0.344136 * cb - 0.714136 * cr, 0.0, 255.0));
            image.samples[i + 2] = uint8_t(std::clamp(y + 1.772 * cb, 0.0, 255.0));
        }
    return image;
}
} // namespace pdf
