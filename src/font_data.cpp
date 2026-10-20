#include "render.h"
#include "font_data.inc"

namespace pdf {
bool builtin_cmap(std::string_view name, CMap& cmap, bool unicode) {
    auto it = std::lower_bound(std::begin(cmap_index), std::end(cmap_index), name,
                               [](const auto& entry, std::string_view key) { return entry.name < key; });
    if (it == std::end(cmap_index) || it->name != name)
        return false;
    static thread_local unsigned nesting = 0;
    require(nesting < 8, "Built-in CMap inheritance limit exceeded");
    ++nesting;
    try {
        auto data = inflate(View(cmap_bytes).subspan(it->offset, it->size));
        parse_cmap(data, cmap, unicode);
        --nesting;
    } catch (...) {
        --nesting;
        throw;
    }
    return true;
}
uint32_t adobe_glyph_unicode(std::string_view name) {
    auto it = std::lower_bound(std::begin(adobe_glyph_names), std::end(adobe_glyph_names), name,
                               [](const auto& entry, std::string_view key) { return entry.name < key; });
    return it == std::end(adobe_glyph_names) || it->name != name ? 0 : it->unicode;
}
} // namespace pdf
