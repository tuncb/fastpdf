#include "image_codecs.h"

namespace pdf {
struct MQState {
    uint16_t qe;
    uint8_t mps, lps;
    bool exchange;
};
static constexpr MQState states[] = {
    {0x5601, 1, 1, true},    {0x3401, 2, 6, false},   {0x1801, 3, 9, false},   {0x0ac1, 4, 12, false},
    {0x0521, 5, 29, false},  {0x0221, 38, 33, false}, {0x5601, 7, 6, true},    {0x5401, 8, 14, false},
    {0x4801, 9, 14, false},  {0x3801, 10, 14, false}, {0x3001, 11, 17, false}, {0x2401, 12, 18, false},
    {0x1c01, 13, 20, false}, {0x1601, 29, 21, false}, {0x5601, 15, 14, true},  {0x5401, 16, 14, false},
    {0x5101, 17, 15, false}, {0x4801, 18, 16, false}, {0x3801, 19, 17, false}, {0x3401, 20, 18, false},
    {0x3001, 21, 19, false}, {0x2801, 22, 19, false}, {0x2401, 23, 20, false}, {0x2201, 24, 21, false},
    {0x1c01, 25, 22, false}, {0x1801, 26, 23, false}, {0x1601, 27, 24, false}, {0x1401, 28, 25, false},
    {0x1201, 29, 26, false}, {0x1101, 30, 27, false}, {0x0ac1, 31, 28, false}, {0x09c1, 32, 29, false},
    {0x08a1, 33, 30, false}, {0x0521, 34, 31, false}, {0x0441, 35, 32, false}, {0x02a1, 36, 33, false},
    {0x0221, 37, 34, false}, {0x0141, 38, 35, false}, {0x0111, 39, 36, false}, {0x0085, 40, 37, false},
    {0x0049, 41, 38, false}, {0x0025, 42, 39, false}, {0x0015, 43, 40, false}, {0x0009, 44, 41, false},
    {0x0005, 45, 42, false}, {0x0001, 45, 43, false}, {0x5601, 46, 46, false}};
static void byte_in(MQDecoder& d) {
    unsigned current = d.position < d.bytes.size() ? d.bytes[d.position] : 255;
    unsigned next = d.position + 1 < d.bytes.size() ? d.bytes[d.position + 1] : 255;
    if (current == 255) {
        if (next > 0x8f) {
            d.c += 0xff00;
            d.count = 8;
        } else {
            ++d.position;
            d.c += next << 9;
            d.count = 7;
        }
    } else {
        ++d.position;
        d.c += next << 8;
        d.count = 8;
    }
}
void mq_init(MQDecoder& d, View bytes) {
    d = {};
    d.bytes = bytes;
    d.c = (bytes.empty() ? 255u : bytes[0]) << 16;
    byte_in(d);
    d.c <<= 7;
    d.count -= 7;
    d.a = 0x8000;
}
unsigned mq_bit(MQDecoder& d, uint8_t& context) {
    require((context >> 1) < std::size(states), "Invalid arithmetic probability state");
    auto state = states[context >> 1];
    unsigned mps = context & 1, bit = mps;
    d.a -= state.qe;
    if ((d.c >> 16) < state.qe) {
        if (d.a < state.qe) {
            bit = mps;
            context = uint8_t((state.mps << 1) | mps);
        } else {
            bit = mps ^ 1;
            context = uint8_t((state.lps << 1) | (mps ^ unsigned(state.exchange)));
        }
        d.a = state.qe;
    } else {
        d.c -= uint32_t(state.qe) << 16;
        if (d.a & 0x8000)
            return mps;
        if (d.a < state.qe) {
            bit = mps ^ 1;
            context = uint8_t((state.lps << 1) | (mps ^ unsigned(state.exchange)));
        } else {
            bit = mps;
            context = uint8_t((state.mps << 1) | mps);
        }
    }
    do {
        if (d.count == 0)
            byte_in(d);
        d.a <<= 1;
        d.c <<= 1;
        --d.count;
    } while (!(d.a & 0x8000));
    return bit;
}
} // namespace pdf
