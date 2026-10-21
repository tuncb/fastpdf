#include "image_codecs.h"

namespace pdf {
struct FaxBits {
    View bytes;
    size_t position = 0;
};
static unsigned fax_bit(FaxBits& bits) {
    require(bits.position / 8 < bits.bytes.size(), "Truncated byte-aligned fax data");
    unsigned value = (bits.bytes[bits.position / 8] >> (7 - bits.position % 8)) & 1;
    ++bits.position;
    return value;
}
// ITU-T T.4 (04/1999), tables 2, 3a and 3b. The first 64 entries are
// terminating runs; the remaining entries are multiples of 64.
static constexpr std::string_view white_codes[] = {
    "00110101", "000111", "0111", "1000", "1011", "1100",
    "1110", "1111", "10011", "10100", "00111", "01000",
    "001000", "000011", "110100", "110101", "101010", "101011",
    "0100111", "0001100", "0001000", "0010111", "0000011", "0000100",
    "0101000", "0101011", "0010011", "0100100", "0011000", "00000010",
    "00000011", "00011010", "00011011", "00010010", "00010011", "00010100",
    "00010101", "00010110", "00010111", "00101000", "00101001", "00101010",
    "00101011", "00101100", "00101101", "00000100", "00000101", "00001010",
    "00001011", "01010010", "01010011", "01010100", "01010101", "00100100",
    "00100101", "01011000", "01011001", "01011010", "01011011", "01001010",
    "01001011", "00110010", "00110011", "00110100", "11011", "10010",
    "010111", "0110111", "00110110", "00110111", "01100100", "01100101",
    "01101000", "01100111", "011001100", "011001101", "011010010", "011010011",
    "011010100", "011010101", "011010110", "011010111", "011011000", "011011001",
    "011011010", "011011011", "010011000", "010011001", "010011010", "011000",
    "010011011",
};
static constexpr std::string_view black_codes[] = {
    "0000110111", "010", "11", "10", "011", "0011",
    "0010", "00011", "000101", "000100", "0000100", "0000101",
    "0000111", "00000100", "00000111", "000011000", "0000010111", "0000011000",
    "0000001000", "00001100111", "00001101000", "00001101100", "00000110111", "00000101000",
    "00000010111", "00000011000", "000011001010", "000011001011", "000011001100", "000011001101",
    "000001101000", "000001101001", "000001101010", "000001101011", "000011010010", "000011010011",
    "000011010100", "000011010101", "000011010110", "000011010111", "000001101100", "000001101101",
    "000011011010", "000011011011", "000001010100", "000001010101", "000001010110", "000001010111",
    "000001100100", "000001100101", "000001010010", "000001010011", "000000100100", "000000110111",
    "000000111000", "000000100111", "000000101000", "000001011000", "000001011001", "000000101011",
    "000000101100", "000001011010", "000001100110", "000001100111", "0000001111", "000011001000",
    "000011001001", "000001011011", "000000110011", "000000110100", "000000110101", "0000001101100",
    "0000001101101", "0000001001010", "0000001001011", "0000001001100", "0000001001101", "0000001110010",
    "0000001110011", "0000001110100", "0000001110101", "0000001110110", "0000001110111", "0000001010010",
    "0000001010011", "0000001010100", "0000001010101", "0000001011010", "0000001011011", "0000001100100",
    "0000001100101",
};
static constexpr std::string_view common_codes[] = {
    "00000001000", "00000001100", "00000001101", "000000010010", "000000010011", "000000010100",
    "000000010101", "000000010110", "000000010111", "000000011100", "000000011101", "000000011110",
    "000000011111",
};

static const std::array<std::array<int16_t, 16384>, 2>& fax_tables() {
    static const auto tables = [] {
        std::array<std::array<int16_t, 16384>, 2> result{};
        for (unsigned color = 0; color < 2; ++color) {
            result[color].fill(-1);
            auto add = [&](std::string_view code, unsigned run) {
                unsigned node = 1;
                for (char bit : code)
                    node = node * 2 + unsigned(bit - '0');
                result[color][node] = int16_t(run);
            };
            for (unsigned i = 0; i < 91; ++i)
                add(color ? black_codes[i] : white_codes[i], i < 64 ? i : (i - 63) * 64);
            for (unsigned i = 0; i < 13; ++i)
                add(common_codes[i], 1792 + i * 64);
        }
        return result;
    }();
    return tables;
}
static unsigned fax_run(FaxBits& bits, unsigned color, unsigned available) {
    unsigned total = 0;
    for (;;) {
        unsigned node = 1;
        int run = -1;
        for (unsigned length = 0; length < 13 && run < 0; ++length) {
            node = node * 2 + fax_bit(bits);
            run = fax_tables()[color][node];
        }
        require(run >= 0 && unsigned(run) <= available - total, "Invalid fax run length");
        total += unsigned(run);
        if (run < 64)
            return total;
    }
}
// Return -3..3 for vertical, 4 for horizontal and 5 for pass mode.
static int fax_mode(FaxBits& bits) {
    unsigned code = 0;
    for (unsigned length = 1; length <= 7; ++length) {
        code = code * 2 + fax_bit(bits);
        if (length == 1 && code == 1)
            return 0;
        if (length == 3) {
            if (code == 1) return 4;
            if (code == 2) return -1;
            if (code == 3) return 1;
        }
        if (length == 4 && code == 1)
            return 5;
        if (length == 6 && (code == 2 || code == 3))
            return code == 2 ? -2 : 2;
        if (length == 7 && (code == 2 || code == 3))
            return code == 2 ? -3 : 3;
    }
    fail("Invalid or unsupported fax mode");
}
BilevelImage decode_mmr_aligned(Document& document, View data, unsigned width, unsigned height) {
    require(width && height && uint64_t(width) * height <= max_stream / 4,
            "Decoded fax image limit exceeded");
    FaxBits bits{data};
    BilevelImage result{width, height, Bytes(size_t(width) * height)};
    std::vector<unsigned> reference{width, width}, changes;
    for (unsigned y = 0; y < height; ++y) {
        if ((y & 63) == 0)
            check_cancel(document);
        auto row = result.pixels.data() + size_t(y) * width;
        unsigned x = 0, color = 0, operations = 0;
        bool first = true;
        while (x < width) {
            require(++operations <= uint64_t(width) * 2 + 16, "Fax row operation limit exceeded");
            auto next = first ? std::lower_bound(reference.begin(), reference.end(), x)
                              : std::upper_bound(reference.begin(), reference.end(), x);
            size_t index = size_t(next - reference.begin());
            if ((index & 1) != color)
                ++index;
            unsigned b1 = index < reference.size() ? reference[index] : width;
            unsigned b2 = index + 1 < reference.size() ? reference[index + 1] : width;
            int mode = fax_mode(bits);
            if (mode == 4) {
                unsigned a1 = x + fax_run(bits, color, width - x);
                unsigned a2 = a1 + fax_run(bits, color ^ 1, width - a1);
                if (color)
                    std::fill(row + x, row + a1, uint8_t(1));
                else
                    std::fill(row + a1, row + a2, uint8_t(1));
                x = a2;
            } else {
                int64_t end = mode == 5 ? b2 : int64_t(b1) + mode;
                require(end >= x && end <= width, "Invalid fax changing element");
                if (color)
                    std::fill(row + x, row + size_t(end), uint8_t(1));
                x = unsigned(end);
                if (mode != 5)
                    color ^= 1;
            }
            first = false;
        }
        changes.clear();
        unsigned previous = 0;
        for (unsigned i = 0; i < width; ++i)
            if (row[i] != previous) {
                changes.push_back(i);
                previous = row[i];
            }
        changes.push_back(width);
        changes.push_back(width);
        reference.swap(changes);
        bits.position = (bits.position + 7) & ~size_t(7);
    }
    return result;
}
} // namespace pdf
