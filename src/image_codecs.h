#pragma once
#include "pdf.h"

namespace pdf {
// ISO/IEC 15444-1 / ITU-T T.800 binary arithmetic decoder. Contexts contain
// the probability-state index in the high bits and the MPS in bit zero.
struct MQDecoder {
    View bytes;
    size_t position = 0;
    uint32_t a = 0x8000, c = 0;
    int count = 0;
};
void mq_init(MQDecoder& decoder, View bytes);
unsigned mq_bit(MQDecoder& decoder, uint8_t& context);
struct JpxImage {
    unsigned width = 0, height = 0, components = 0;
    unsigned source_width = 0, source_height = 0, reduction = 0;
    int alpha = -1;
    bool premultiplied = false;
    Bytes samples;
};
struct JpxProfile {
    double packets_ms = 0, blocks_ms = 0, wavelet_ms = 0, output_ms = 0;
};
struct JpxOptions {
    unsigned workers = 0;
    JpxProfile* profile = nullptr;
    unsigned reduction = 0;
    bool indexed = false;
};
JpxImage decode_jpx(Document& document, View data, JpxOptions options = {});
struct Renderer;
struct BilevelImage {
    unsigned width = 0, height = 0;
    Bytes pixels;
}; // 0 white, 1 black
BilevelImage decode_mmr(Renderer& renderer, View data, unsigned width, unsigned height);
BilevelImage decode_jbig2(Renderer& renderer, Document& document, View data, View globals, unsigned width,
                          unsigned height);
} // namespace pdf
