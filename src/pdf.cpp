#include "pdf.h"
#include <charconv>

namespace pdf {
static const Value nil;
[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}
void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}
View view(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
std::string string(View b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}
bool whitespace(uint8_t c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
bool delimiter(uint8_t c) {
    return whitespace(c) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}
void skip_space(Parser& p) {
    while (p.pos < p.data.size()) {
        if (whitespace(p.data[p.pos])) {
            ++p.pos;
            continue;
        }
        if (p.data[p.pos] == '%') {
            while (p.pos < p.data.size() && p.data[p.pos] != '\r' && p.data[p.pos] != '\n')
                ++p.pos;
            continue;
        }
        break;
    }
}
std::string_view token(Parser& p) {
    skip_space(p);
    size_t start = p.pos;
    while (p.pos < p.data.size() && !delimiter(p.data[p.pos]))
        ++p.pos;
    return {reinterpret_cast<const char*>(p.data.data() + start), p.pos - start};
}
static int hex(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
Value parse(Parser& p, int depth) {
    require(depth < 128 && ++p.nodes <= max_nodes, "PDF object nesting or allocation limit exceeded");
    skip_space(p);
    require(p.pos < p.data.size(), "Unexpected end of PDF object");
    Value v;
    auto c = p.data[p.pos++];
    if (c == '/') {
        v.kind = Kind::name;
        while (p.pos < p.data.size() && !delimiter(p.data[p.pos])) {
            uint8_t n = p.data[p.pos++];
            if (n == '#' && p.pos + 1 < p.data.size() && hex(p.data[p.pos]) >= 0 &&
                hex(p.data[p.pos + 1]) >= 0) {
                n = uint8_t(hex(p.data[p.pos]) * 16 + hex(p.data[p.pos + 1]));
                p.pos += 2;
            }
            v.text.push_back(char(n));
        }
    } else if (c == '(') {
        v.kind = Kind::string;
        int nesting = 1;
        while (p.pos < p.data.size() && nesting) {
            c = p.data[p.pos++];
            if (c == '\\') {
                require(p.pos < p.data.size(), "Unterminated PDF string escape");
                c = p.data[p.pos++];
                if (c == 'n')
                    c = '\n';
                else if (c == 'r')
                    c = '\r';
                else if (c == 't')
                    c = '\t';
                else if (c == 'b')
                    c = '\b';
                else if (c == 'f')
                    c = '\f';
                else if (c == '\r' || c == '\n') {
                    if (c == '\r' && p.pos < p.data.size() && p.data[p.pos] == '\n')
                        ++p.pos;
                    continue;
                } else if (c >= '0' && c <= '7') {
                    unsigned n = c - '0';
                    for (int k = 0;
                         k < 2 && p.pos < p.data.size() && p.data[p.pos] >= '0' && p.data[p.pos] <= '7'; ++k)
                        n = n * 8 + p.data[p.pos++] - '0';
                    c = uint8_t(n);
                }
            } else if (c == '(') {
                require(++nesting < 128, "String nesting limit exceeded");
            } else if (c == ')') {
                if (--nesting == 0)
                    break;
            } else if (c == '\r') {
                c = '\n';
                if (p.pos < p.data.size() && p.data[p.pos] == '\n')
                    ++p.pos;
            }
            v.text.push_back(char(c));
            require(v.text.size() <= max_stream, "PDF string is too large");
        }
        require(nesting == 0, "Unterminated PDF string");
    } else if (c == '[') {
        v.kind = Kind::array;
        for (;;) {
            skip_space(p);
            require(p.pos < p.data.size(), "Unterminated PDF array");
            if (p.data[p.pos] == ']') {
                ++p.pos;
                break;
            }
            v.array.push_back(parse(p, depth + 1));
        }
    } else if (c == '<' && p.pos < p.data.size() && p.data[p.pos] == '<') {
        ++p.pos;
        v.kind = Kind::dict;
        for (;;) {
            skip_space(p);
            require(p.pos + 1 < p.data.size(), "Unterminated PDF dictionary");
            if (p.data[p.pos] == '>' && p.data[p.pos + 1] == '>') {
                p.pos += 2;
                break;
            }
            Value key = parse(p, depth + 1);
            require(key.kind == Kind::name, "PDF dictionary key is not a name");
            Value item = parse(p, depth + 1);
            auto it =
                std::find_if(v.dict.begin(), v.dict.end(), [&](auto& e) { return e.first == key.text; });
            if (it == v.dict.end())
                v.dict.emplace_back(std::move(key.text), std::move(item));
            else
                it->second = std::move(item);
        }
    } else if (c == '<') {
        v.kind = Kind::string;
        int high = -1;
        bool ended = false;
        while (p.pos < p.data.size()) {
            c = p.data[p.pos++];
            if (c == '>') {
                ended = true;
                break;
            }
            if (whitespace(c))
                continue;
            int h = hex(c);
            require(h >= 0, "Invalid hexadecimal string");
            if (high < 0)
                high = h;
            else {
                v.text.push_back(char(high * 16 + h));
                high = -1;
            }
        }
        require(ended, "Unterminated hexadecimal string");
        if (high >= 0)
            v.text.push_back(char(high * 16));
    } else {
        --p.pos;
        auto t = token(p);
        require(!t.empty(), "Unexpected PDF delimiter");
        if (t == "null")
            return v;
        if (t == "true" || t == "false") {
            v.kind = Kind::boolean;
            v.number = t == "true";
            return v;
        }
        auto number_text = t;
        if (number_text[0] == '+')
            number_text.remove_prefix(1);
        double n = 0;
        auto r = std::from_chars(number_text.data(), number_text.data() + number_text.size(), n);
        if (r.ec == std::errc() && r.ptr == number_text.data() + number_text.size() && std::isfinite(n)) {
            v.kind = Kind::number;
            v.number = n;
            if (p.references && n >= 0 && n < max_objects &&
                t.find_first_of(".+-") == std::string_view::npos) {
                size_t save = p.pos;
                auto gen = token(p);
                unsigned g = 0;
                auto gr = std::from_chars(gen.data(), gen.data() + gen.size(), g);
                if (gr.ec == std::errc() && gr.ptr == gen.data() + gen.size() && g <= 65535 &&
                    token(p) == "R") {
                    v.kind = Kind::ref;
                    v.object = uint32_t(n);
                    v.generation = g;
                } else
                    p.pos = save;
            }
        } else {
            v.kind = Kind::keyword;
            v.text = t;
        }
    }
    return v;
}
const Value& get(const Value& v, std::string_view key) {
    for (auto& e : v.dict)
        if (e.first == key)
            return e.second;
    return nil;
}
const Value& at(const Value& v, size_t i) {
    return i < v.array.size() ? v.array[i] : nil;
}
double num(const Value& v, double fallback) {
    return v.kind == Kind::number ? v.number : fallback;
}
int integer(const Value& v, int fallback) {
    return v.kind == Kind::number && v.number >= INT_MIN && v.number <= INT_MAX ? int(v.number) : fallback;
}
bool boolean(const Value& v, bool fallback) {
    return v.kind == Kind::boolean ? v.number != 0 : fallback;
}
bool named(const Value& v, std::string_view name) {
    return v.kind == Kind::name && v.text == name;
}
Value reference(uint32_t id, uint32_t generation) {
    Value v;
    v.kind = Kind::ref;
    v.object = id;
    v.generation = generation;
    return v;
}
std::wstring utf8(std::string_view t) {
    if (t.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), r.data(), n);
    return r;
}
std::string utf8(std::wstring_view t) {
    if (t.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, t.data(), int(t.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring pdf_text(std::string_view t) {
    std::wstring out;
    if (t.size() >= 2 &&
        ((uint8_t(t[0]) == 254 && uint8_t(t[1]) == 255) || (uint8_t(t[0]) == 255 && uint8_t(t[1]) == 254))) {
        bool big = uint8_t(t[0]) == 254;
        for (size_t i = 2; i + 1 < t.size(); i += 2)
            out.push_back(wchar_t(big ? (uint8_t(t[i]) * 256 + uint8_t(t[i + 1]))
                                      : (uint8_t(t[i + 1]) * 256 + uint8_t(t[i]))));
    } else if (t.starts_with("\xef\xbb\xbf"))
        out = utf8(t.substr(3));
    else {
        constexpr wchar_t special[] = {0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044,
                                       0x2039, 0x203a, 0x2212, 0x2030, 0x201e, 0x201c, 0x201d, 0x2018,
                                       0x2019, 0x201a, 0x2122, 0xfb01, 0xfb02, 0x0141, 0x0152, 0x0160,
                                       0x0178, 0x017d, 0x0131, 0x0142, 0x0153, 0x0161, 0x017e, 0xfffd};
        for (uint8_t c : t)
            out.push_back(c >= 128 && c <= 159 ? special[c - 128] : c == 160 ? 0x20ac : wchar_t(c));
    }
    return out;
}
Matrix multiply(Matrix o, Matrix i) {
    return {o.a * i.a + o.c * i.b, o.b * i.a + o.d * i.b,       o.a * i.c + o.c * i.d,
            o.b * i.c + o.d * i.d, o.a * i.e + o.c * i.f + o.e, o.b * i.e + o.d * i.f + o.f};
}
Point transform(Matrix m, Point p) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}
Matrix inverse(Matrix m) {
    double det = m.a * m.d - m.b * m.c;
    require(std::abs(det) > 1e-16, "Singular transformation");
    return {m.d / det,
            -m.b / det,
            -m.c / det,
            m.a / det,
            (m.c * m.f - m.d * m.e) / det,
            (m.b * m.e - m.a * m.f) / det};
}
Rect rectangle(const Value& v, Rect f) {
    if (v.array.size() != 4)
        return f;
    Rect r{num(v.array[0]), num(v.array[1]), num(v.array[2]), num(v.array[3])};
    if (r.x0 > r.x1)
        std::swap(r.x0, r.x1);
    if (r.y0 > r.y1)
        std::swap(r.y0, r.y1);
    return r;
}
void map_file(Mapping& m, const std::filesystem::path& path) {
    m.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(m.file != INVALID_HANDLE_VALUE, "Cannot open the PDF file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(m.file, &size) || size.QuadPart <= 0 || uint64_t(size.QuadPart) > SIZE_MAX) {
        unmap_file(m);
        fail("Empty or oversized PDF file");
    }
    m.mapping = CreateFileMappingW(m.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m.mapping) {
        unmap_file(m);
        fail("Cannot create file mapping");
    }
    auto ptr = static_cast<const uint8_t*>(MapViewOfFile(m.mapping, FILE_MAP_READ, 0, 0, 0));
    if (!ptr) {
        unmap_file(m);
        fail("Cannot map PDF into memory");
    }
    m.bytes = {ptr, size_t(size.QuadPart)};
}
void unmap_file(Mapping& m) {
    if (m.bytes.data())
        UnmapViewOfFile(m.bytes.data());
    if (m.mapping)
        CloseHandle(m.mapping);
    if (m.file != INVALID_HANDLE_VALUE)
        CloseHandle(m.file);
    m = {};
}
void warn(Document& doc, std::string s) {
    if (doc.warnings.size() < 128 &&
        std::find(doc.warnings.begin(), doc.warnings.end(), s) == doc.warnings.end())
        doc.warnings.push_back(std::move(s));
}
void check_cancel(Document& doc) {
    if (doc.cancel && doc.cancel->load(std::memory_order_relaxed))
        fail("Operation cancelled");
}
void close(Document& doc) {
    doc.objects.clear();
    doc.resolving.clear();
    doc.object_streams.clear();
    doc.function_samples.clear();
    doc.calculators.clear();
    doc.function_cache_bytes = 0;
    doc.xref.clear();
    doc.pages.clear();
    doc.outline.clear();
    doc.trailer = {};
    doc.catalog = {};
    doc.security = {};
    doc.warnings.clear();
    doc.parsed_nodes = 0;
    doc.repaired = false;
    unmap_file(doc.file);
}
static uint64_t unsigned_token(Parser& p) {
    auto t = token(p);
    uint64_t n = 0;
    auto r = std::from_chars(t.data(), t.data() + t.size(), n);
    require(r.ec == std::errc() && r.ptr == t.data() + t.size(), "Invalid PDF integer");
    return n;
}
static Value indirect(Document& doc, uint64_t offset, uint32_t* object = nullptr,
                      uint32_t* generation = nullptr) {
    require(offset < doc.file.bytes.size(), "Object offset is outside the PDF file");
    Parser p{doc.file.bytes, size_t(offset)};
    uint64_t id = unsigned_token(p), gen = unsigned_token(p);
    require(id < max_objects && gen <= 65535 && token(p) == "obj", "Invalid indirect object header");
    if (object)
        *object = uint32_t(id);
    if (generation)
        *generation = uint32_t(gen);
    Value v = parse(p);
    doc.parsed_nodes += p.nodes;
    require(doc.parsed_nodes <= max_nodes * 8, "PDF object allocation limit exceeded");
    size_t save = p.pos;
    if (v.kind == Kind::dict && token(p) == "stream") {
        if (p.pos < p.data.size() && p.data[p.pos] == '\r')
            ++p.pos;
        if (p.pos < p.data.size() && p.data[p.pos] == '\n')
            ++p.pos;
        else if (p.pos < p.data.size() && p.data[p.pos] == ' ')
            ++p.pos;
        const Value* len = &get(v, "Length");
        if (len->kind == Kind::ref && !doc.xref.empty())
            len = &resolve(doc, *len);
        double length = num(*len, -1);
        if (length >= 0 && length <= double(p.data.size() - p.pos))
            v.stream = p.data.subspan(p.pos, size_t(length));
        else {
            std::string_view rest(reinterpret_cast<const char*>(p.data.data() + p.pos),
                                  p.data.size() - p.pos);
            size_t end = rest.find("endstream");
            require(end != std::string_view::npos, "Unterminated PDF stream");
            if (end && rest[end - 1] == '\n')
                --end;
            if (end && rest[end - 1] == '\r')
                --end;
            v.stream = p.data.subspan(p.pos, end);
            warn(doc, "Recovered a stream with an invalid Length");
        }
        v.has_stream = true;
    } else
        p.pos = save;
    return v;
}
static void put_xref(Document& d, uint32_t id, Xref e, bool overwrite = false) {
    require(id < max_objects, "PDF object number limit exceeded");
    if (d.xref.size() <= id)
        d.xref.resize(size_t(id) + 1);
    if (overwrite || d.xref[id].type == 255)
        d.xref[id] = e;
}
static void merge_trailer(Document& d, const Value& v) {
    if (d.trailer.kind != Kind::dict)
        d.trailer.kind = Kind::dict;
    for (auto& e : v.dict)
        if (get(d.trailer, e.first).kind == Kind::null)
            d.trailer.dict.push_back(e);
}
static void load_xref(Document& d, uint64_t offset, std::unordered_set<uint64_t>& visited, int depth = 0,
                      const std::unordered_set<uint32_t>* hybrid_entries = nullptr) {
    require(depth < 128, "Too many incremental PDF revisions");
    if (!visited.insert(offset).second)
        return;
    require(offset < d.file.bytes.size(), "Cross-reference offset is outside file");
    Parser p{d.file.bytes, size_t(offset)};
    Value trailer;
    std::unordered_set<uint32_t> revision_entries;
    if (token(p) == "xref") {
        for (;;) {
            size_t saved = p.pos;
            auto t = token(p);
            if (t == "trailer") {
                trailer = parse(p);
                break;
            }
            p.pos = saved;
            uint64_t start = unsigned_token(p), count = unsigned_token(p);
            require(start < max_objects && count <= max_objects - start, "Invalid xref subsection");
            for (uint64_t j = 0; j < count; ++j) {
                uint64_t off = unsigned_token(p), gen = unsigned_token(p);
                auto state = token(p);
                require(gen <= 65535 && (state == "n" || state == "f"), "Invalid xref entry");
                uint32_t id = uint32_t(start + j);
                if (id >= d.xref.size() || d.xref[id].type == 255)
                    revision_entries.insert(id);
                put_xref(d, id, {off, 0, uint32_t(gen), uint8_t(state == "n" ? 1 : 0)});
            }
        }
    } else {
        trailer = indirect(d, offset);
        require(named(get(trailer, "Type"), "XRef"), "Expected a cross-reference stream");
        auto w = get(trailer, "W");
        require(w.array.size() == 3, "Invalid cross-reference field widths");
        int widths[3];
        size_t stride = 0;
        for (int i = 0; i < 3; ++i) {
            widths[i] = integer(w.array[i], -1);
            require(widths[i] >= 0 && widths[i] <= 8, "Invalid xref width");
            stride += widths[i];
        }
        require(stride > 0, "Empty xref record");
        Bytes bytes = decode_stream(d, trailer).bytes;
        size_t cursor = 0;
        auto index = get(trailer, "Index");
        if (index.kind == Kind::null) {
            index.kind = Kind::array;
            Value a;
            a.kind = Kind::number;
            index.array.push_back(a);
            index.array.push_back(get(trailer, "Size"));
        }
        require(index.array.size() % 2 == 0, "Invalid xref Index");
        for (size_t i = 0; i < index.array.size(); i += 2) {
            int start = integer(index.array[i], -1), count = integer(index.array[i + 1], -1);
            require(start >= 0 && count >= 0 && uint64_t(start) + count <= max_objects,
                    "Invalid xref Index range");
            require(size_t(count) <= (bytes.size() - cursor) / stride, "Truncated xref stream");
            for (int j = 0; j < count; ++j) {
                uint64_t fields[3] = {widths[0] ? 0u : 1u, 0, 0};
                for (int k = 0; k < 3; ++k)
                    for (int b = 0; b < widths[k]; ++b)
                        fields[k] = (fields[k] << 8) | bytes[cursor++];
                if (fields[0] <= 2) {
                    require(fields[2] <= UINT32_MAX, "Invalid xref generation/index");
                    uint32_t id = uint32_t(start + j);
                    put_xref(d, id,
                             {fields[1], fields[0] == 2 ? uint32_t(fields[2]) : 0,
                              fields[0] == 2 ? 0 : uint32_t(fields[2]), uint8_t(fields[0])},
                             hybrid_entries && hybrid_entries->contains(id));
                }
            }
        }
    }
    merge_trailer(d, trailer);
    auto supplement = num(get(trailer, "XRefStm"), -1);
    if (supplement >= 0)
        load_xref(d, uint64_t(supplement), visited, depth + 1, &revision_entries);
    auto prev = num(get(trailer, "Prev"), -1);
    if (prev >= 0)
        load_xref(d, uint64_t(prev), visited, depth + 1);
}
static void repair(Document& d) {
    d.xref.clear();
    d.objects.clear();
    d.trailer = {};
    d.repaired = true;
    warn(d, "Rebuilt damaged cross-reference information by scanning the file");
    auto data = d.file.bytes;
    size_t pos = 0;
    std::string_view whole(reinterpret_cast<const char*>(data.data()), data.size());
    while (pos < data.size()) {
        check_cancel(d);
        if (data[pos] >= '0' && data[pos] <= '9' && (pos == 0 || whitespace(data[pos - 1]))) {
            Parser p{data, pos};
            try {
                uint64_t id = unsigned_token(p), gen = unsigned_token(p);
                if (id > 0 && id < max_objects && gen <= 65535 && token(p) == "obj") {
                    put_xref(d, uint32_t(id), {pos, 0, uint32_t(gen), 1}, true);
                    Value v = indirect(d, pos);
                    if (named(get(v, "Type"), "Catalog")) {
                        Value tr;
                        tr.kind = Kind::dict;
                        tr.dict.emplace_back("Root", reference(uint32_t(id), uint32_t(gen)));
                        d.trailer = std::move(tr);
                    }
                    if (named(get(v, "Type"), "XRef"))
                        merge_trailer(d, v);
                    if (v.has_stream)
                        pos = size_t(v.stream.data() - data.data()) + v.stream.size();
                    else
                        pos = p.pos;
                    size_t end = whole.find("endobj", pos);
                    if (end != std::string_view::npos)
                        pos = end + 6;
                    continue;
                }
            } catch (const std::exception&) {
            }
        }
        if (whole.substr(pos, 7) == "trailer") {
            try {
                Parser p{data, pos + 7};
                Value tr = parse(p);
                if (tr.kind == Kind::dict) {
                    for (auto& e : d.trailer.dict)
                        if (get(tr, e.first).kind == Kind::null)
                            tr.dict.push_back(e);
                    d.trailer = std::move(tr);
                }
            } catch (const std::exception&) {
            }
        }
        ++pos;
    }
    require(!d.xref.empty(), "No readable PDF objects found");
}
const Value& resolve(Document& d, const Value& input) {
    if (input.kind != Kind::ref)
        return input;
    uint32_t id = input.object;
    check_cancel(d);
    if (id >= d.xref.size() || d.xref[id].type == 0 || d.xref[id].type == 255) {
        warn(d, "Missing indirect object " + std::to_string(id));
        return nil;
    }
    auto e = d.xref[id];
    if (e.generation != input.generation) {
        warn(d, "Stale indirect reference " + std::to_string(id));
        return nil;
    }
    auto found = d.objects.find(id);
    if (found != d.objects.end())
        return *found->second;
    require(d.resolving.size() < 128 && d.resolving.insert(id).second, "Cyclic indirect object reference");
    try {
        if (e.type == 1) {
            uint32_t parsed_id = 0, gen = 0;
            Value v = indirect(d, e.offset, &parsed_id, &gen);
            require(parsed_id == id && gen == e.generation, "Cross-reference points at a different object");
            if (d.security.enabled && id != get(d.trailer, "Encrypt").object &&
                !named(get(v, "Type"), "XRef"))
                decrypt_strings(d, v, id, gen);
            v.object = id;
            v.generation = gen;
            d.objects[id] = std::make_unique<Value>(std::move(v));
        } else if (e.type == 2) {
            require(e.offset < max_objects, "Invalid object stream reference");
            auto& stream = resolve(d, reference(uint32_t(e.offset)));
            require(named(get(stream, "Type"), "ObjStm"), "Compressed object references a non-object stream");
            auto bytes = decode_stream(d, stream).bytes;
            int count = integer(get(stream, "N"), -1), first = integer(get(stream, "First"), -1);
            require(count >= 0 && count <= 100000 && first >= 0 && size_t(first) <= bytes.size(),
                    "Invalid object stream header");
            Parser p{bytes};
            std::vector<std::pair<uint32_t, size_t>> table;
            table.reserve(count);
            for (int j = 0; j < count; ++j) {
                uint64_t object = unsigned_token(p), off = unsigned_token(p);
                require(object < max_objects && off < bytes.size() - first, "Invalid object stream index");
                table.emplace_back(uint32_t(object), size_t(first) + size_t(off));
            }
            require(p.pos <= size_t(first), "Object stream header overlaps its objects");
            require(e.index < table.size() && table[e.index].first == id, "Object stream index mismatch");
            for (size_t j = 0; j < table.size(); ++j) {
                auto [obj, off] = table[j];
                if (obj >= d.xref.size() || d.xref[obj].type != 2 || d.xref[obj].offset != e.offset ||
                    d.xref[obj].index != j || d.objects.contains(obj))
                    continue;
                Parser q{bytes, off};
                auto v = parse(q);
                d.parsed_nodes += q.nodes;
                require(d.parsed_nodes <= max_nodes * 8, "Object allocation limit exceeded");
                v.object = obj;
                d.objects[obj] = std::make_unique<Value>(std::move(v));
            }
        } else
            fail("Unsupported cross-reference entry");
        d.resolving.erase(id);
        auto it = d.objects.find(id);
        require(it != d.objects.end(), "Object missing from object stream");
        return *it->second;
    } catch (...) {
        d.resolving.erase(id);
        throw;
    }
}
const Value& entry(Document& d, const Value& v, std::string_view key) {
    return resolve(d, get(resolve(d, v), key));
}
static void pages(Document& d, const Value& ref, Value resources, Rect media, Rect crop, int rotation,
                  std::unordered_set<uint32_t>& path, int depth) {
    require(depth < 128 && d.pages.size() < 200000, "Page tree limit exceeded");
    check_cancel(d);
    if (ref.kind == Kind::ref)
        require(path.insert(ref.object).second, "Cyclic page tree");
    auto& node = resolve(d, ref);
    require(node.kind == Kind::dict, "Invalid page tree node");
    if (get(node, "Resources").kind != Kind::null)
        resources = get(node, "Resources");
    if (get(node, "MediaBox").kind != Kind::null)
        media = rectangle(entry(d, node, "MediaBox"), media);
    if (get(node, "CropBox").kind != Kind::null)
        crop = rectangle(entry(d, node, "CropBox"), media);
    if (get(node, "Rotate").kind != Kind::null)
        rotation = integer(entry(d, node, "Rotate"));
    if (named(get(node, "Type"), "Page") || get(node, "Kids").kind == Kind::null) {
        Rect box{std::max(media.x0, crop.x0), std::max(media.y0, crop.y0), std::min(media.x1, crop.x1),
                 std::min(media.y1, crop.y1)};
        if (box.x1 <= box.x0 || box.y1 <= box.y0)
            box = media;
        require(box.x1 > box.x0 && box.y1 > box.y0, "Page has invalid dimensions");
        double unit = num(entry(d, node, "UserUnit"), 1);
        require(unit > 0 && unit <= 75000, "Invalid UserUnit");
        rotation = ((rotation % 360) + 360) % 360;
        if (rotation % 90) {
            warn(d, "Invalid page rotation ignored");
            rotation = 0;
        }
        d.pages.push_back({ref, resources, get(node, "Contents"), get(node, "Annots"), box, rotation, unit});
    } else {
        auto& kids = entry(d, node, "Kids");
        require(kids.kind == Kind::array, "Invalid page Kids");
        for (auto& child : kids.array)
            pages(d, child, resources, media, crop, rotation, path, depth + 1);
    }
    if (ref.kind == Kind::ref)
        path.erase(ref.object);
}
static const Value& named_dest(Document& d, const Value& tree, std::string_view name, int depth = 0) {
    if (depth > 64)
        return nil;
    auto& node = resolve(d, tree);
    auto& names = entry(d, node, "Names");
    for (size_t i = 0; i + 1 < names.array.size(); i += 2)
        if (names.array[i].text == name)
            return names.array[i + 1];
    auto& kids = entry(d, node, "Kids");
    for (auto& child : kids.array) {
        auto& found = named_dest(d, child, name, depth + 1);
        if (found.kind != Kind::null)
            return found;
    }
    return nil;
}
int destination_page(Document& d, const Value& value) {
    const Value* dest = &resolve(d, value);
    if (dest->kind == Kind::name || dest->kind == Kind::string) {
        auto& legacy = entry(d, d.catalog, "Dests");
        auto& old = get(legacy, dest->text);
        dest = old.kind != Kind::null
                   ? &resolve(d, old)
                   : &resolve(d, named_dest(d, get(entry(d, d.catalog, "Names"), "Dests"), dest->text));
    }
    if (dest->kind == Kind::dict)
        dest = &entry(d, *dest, "D");
    auto& page = at(*dest, 0);
    if (page.kind == Kind::ref) {
        for (size_t i = 0; i < d.pages.size(); ++i)
            if (d.pages[i].reference.object == page.object)
                return int(i);
    }
    if (page.kind == Kind::number) {
        int p = integer(page, -1);
        if (p >= 0 && size_t(p) < d.pages.size())
            return p;
    }
    return -1;
}
static void outlines(Document& d, Value item, int depth, std::unordered_set<uint32_t>& visited) {
    while (item.kind == Kind::ref && depth < 64 && d.outline.size() < 10000 &&
           visited.insert(item.object).second) {
        auto& node = resolve(d, item);
        auto dest = get(node, "Dest");
        if (dest.kind == Kind::null)
            dest = get(entry(d, node, "A"), "D");
        d.outline.push_back({pdf_text(entry(d, node, "Title").text), destination_page(d, dest), depth});
        outlines(d, get(node, "First"), depth + 1, visited);
        item = get(node, "Next");
    }
}
void open(Document& d, const std::filesystem::path& path, std::string_view password) {
    close(d);
    d.path = path.wstring();
    try {
        map_file(d.file, path);
        auto header = d.file.bytes.first(std::min<size_t>(1024, d.file.bytes.size()));
        require(string(header).find("%PDF-") != std::string::npos, "This file does not contain a PDF header");
        auto tail = d.file.bytes.last(std::min<size_t>(65536, d.file.bytes.size()));
        auto end = string(tail);
        size_t mark = end.rfind("startxref");
        try {
            require(mark != std::string::npos, "Missing startxref");
            Parser p{view(end), mark + 9};
            auto offset = unsigned_token(p);
            std::unordered_set<uint64_t> visited;
            load_xref(d, offset, visited);
        } catch (const std::exception&) {
            check_cancel(d);
            repair(d);
        }
        if (get(d.trailer, "Encrypt").kind != Kind::null)
            authenticate(d, password);
        d.catalog = resolve(d, get(d.trailer, "Root"));
        require(d.catalog.kind == Kind::dict, "No readable PDF document catalog");
        std::unordered_set<uint32_t> visited;
        Rect uncropped{-1e30, -1e30, 1e30, 1e30};
        pages(d, get(d.catalog, "Pages"), {}, Rect{}, uncropped, 0, visited, 0);
        require(!d.pages.empty(), "The PDF contains no pages");
        visited.clear();
        outlines(d, get(entry(d, d.catalog, "Outlines"), "First"), 0, visited);
        if (get(entry(d, d.catalog, "AcroForm"), "XFA").kind != Kind::null)
            warn(d, "XFA dynamic forms are not supported; only stored page appearances are displayed");
    } catch (...) {
        unmap_file(d.file);
        throw;
    }
}
Bytes page_content(Document& d, const Page& p) {
    Bytes result;
    auto& v = resolve(d, p.contents);
    auto append = [&](const Value& item) {
        auto& stream = resolve(d, item);
        if (stream.kind == Kind::null)
            return;
        auto part = decode_stream(d, stream).bytes;
        require(part.size() <= max_stream - result.size() - 1, "Page content limit exceeded");
        result.insert(result.end(), part.begin(), part.end());
        result.push_back('\n');
    };
    if (v.kind == Kind::array)
        for (auto& item : v.array)
            append(item);
    else if (v.kind != Kind::null)
        append(v);
    return result;
}
} // namespace pdf
