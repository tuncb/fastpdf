#include "render.h"
#include <charconv>

namespace pdf {
#include "font_encodings.inc"
static uint32_t big_code(std::string_view s) {
    require(s.size() <= 4, "CMap code exceeds four bytes");
    uint32_t n = 0;
    for (uint8_t c : s)
        n = (n << 8) | c;
    return n;
}
static std::wstring utf16be(std::string_view s) {
    std::wstring out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(wchar_t(uint8_t(s[i]) * 256 + uint8_t(s[i + 1])));
    return out;
}
void parse_cmap(View bytes, CMap& cmap, bool unicode) {
    Parser p{bytes, 0, 0, false};
    Value previous;
    while (p.pos < p.data.size()) {
        skip_space(p);
        if (p.pos == p.data.size())
            break;
        auto v = parse(p);
        if (v.kind != Kind::keyword) {
            previous = std::move(v);
            continue;
        }
        int count = integer(previous);
        require(count >= 0 && count <= 100000, "CMap entry limit exceeded");
        if (v.text == "begincodespacerange") {
            for (int i = 0; i < count; ++i) {
                auto a = parse(p), b = parse(p);
                require(a.kind == Kind::string && a.text.size() == b.text.size() && !a.text.empty(),
                        "Invalid CMap code space");
                cmap.ranges.push_back({big_code(a.text), big_code(b.text), uint8_t(a.text.size())});
            }
        } else if (v.text == "beginbfchar" || v.text == "begincidchar") {
            for (int i = 0; i < count; ++i) {
                auto a = parse(p), b = parse(p);
                uint32_t code = big_code(a.text);
                if (v.text == "beginbfchar")
                    cmap.unicode[code] = utf16be(b.text);
                else
                    cmap.cid[code] = uint32_t(std::max(0, integer(b)));
            }
        } else if (v.text == "beginbfrange" || v.text == "begincidrange") {
            for (int i = 0; i < count; ++i) {
                auto a = parse(p), b = parse(p), dest = parse(p);
                uint32_t lo = big_code(a.text), hi = big_code(b.text);
                require(hi >= lo && hi - lo <= 65535, "Invalid CMap range");
                for (uint32_t n = 0; n <= hi - lo; ++n) {
                    if (v.text == "begincidrange")
                        cmap.cid[lo + n] = uint32_t(std::max(0, integer(dest))) + n;
                    else if (dest.kind == Kind::array)
                        cmap.unicode[lo + n] = utf16be(at(dest, n).text);
                    else {
                        cmap.unicode[lo + n] = utf16be(dest.text);
                        for (size_t k = dest.text.size(); k > 0; --k) {
                            auto c = uint8_t(dest.text[k - 1]) + 1;
                            dest.text[k - 1] = char(c);
                            if (c < 256)
                                break;
                        }
                    }
                }
                require(cmap.unicode.size() + cmap.cid.size() <= 1000000, "CMap allocation limit exceeded");
            }
        } else if (v.text == "usecmap") {
            if (previous.text == "Identity-H" || previous.text == "Identity-V") {
                cmap.identity = true;
                cmap.vertical = previous.text == "Identity-V";
                cmap.ranges.push_back({0, 65535, 2});
            } else
                require(builtin_cmap(previous.text, cmap, unicode),
                        "Unknown inherited CMap: " + previous.text);
        } else if (v.text == "def" &&
                   previous.kind == Kind::number) { /* WMode is handled by the token check below. */
        }
        previous = {};
    }
    auto text = string(bytes);
    if (text.find("/WMode 1") != std::string::npos)
        cmap.vertical = true;
    if (!unicode && cmap.ranges.empty() && cmap.identity)
        cmap.ranges.push_back({0, 65535, 2});
}
static const std::vector<std::string>& standard_names() {
    static const auto names = [] {
        std::string s =
            ".notdef space exclam quotedbl numbersign dollar percent ampersand quoteright parenleft "
            "parenright asterisk plus comma hyphen period slash zero one two three four five six seven eight "
            "nine colon semicolon less equal greater question at A B C D E F G H I J K L M N O P Q R S T U V "
            "W X Y Z bracketleft backslash bracketright asciicircum underscore quoteleft a b c d e f g h i j "
            "k l m n o p q r s t u v w x y z braceleft bar braceright asciitilde exclamdown cent sterling "
            "fraction yen florin section currency quotesingle quotedblleft guillemotleft guilsinglleft "
            "guilsinglright fi fl endash dagger daggerdbl periodcentered paragraph bullet quotesinglbase "
            "quotedblbase quotedblright guillemotright ellipsis perthousand questiondown grave acute "
            "circumflex tilde macron breve dotaccent dieresis ring cedilla hungarumlaut ogonek caron emdash "
            "AE ordfeminine Lslash Oslash OE ordmasculine ae dotlessi lslash oslash oe germandbls "
            "onesuperior logicalnot mu trademark Eth onehalf plusminus Thorn onequarter divide brokenbar "
            "degree thorn threequarters twosuperior registered minus eth multiply threesuperior copyright "
            "Aacute Acircumflex Adieresis Agrave Aring Atilde Ccedilla Eacute Ecircumflex Edieresis Egrave "
            "Iacute Icircumflex Idieresis Igrave Ntilde Oacute Ocircumflex Odieresis Ograve Otilde Scaron "
            "Uacute Ucircumflex Udieresis Ugrave Yacute Ydieresis Zcaron aacute acircumflex adieresis agrave "
            "aring atilde ccedilla eacute ecircumflex edieresis egrave iacute icircumflex idieresis igrave "
            "ntilde oacute ocircumflex odieresis ograve otilde scaron uacute ucircumflex udieresis ugrave "
            "yacute ydieresis zcaron";
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size()) {
            auto end = s.find(' ', start);
            if (end == std::string::npos)
                end = s.size();
            out.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }();
    return names;
}
uint32_t glyph_unicode(std::string_view name) {
    if (auto dot = name.find('.'); dot != std::string_view::npos)
        name = name.substr(0, dot);
    if (name.size() == 1)
        return uint8_t(name[0]);
    if ((name.starts_with("uni") && name.size() >= 7) ||
        (name.starts_with('u') && name.size() >= 5 && name.size() <= 7)) {
        auto h = name.substr(name.starts_with("uni") ? 3 : 1, name.starts_with("uni") ? 4 : 6);
        uint32_t n = 0;
        auto r = std::from_chars(h.data(), h.data() + h.size(), n, 16);
        if (r.ec == std::errc() && r.ptr == h.data() + h.size() && n <= 0x10ffff)
            return n;
    }
    if (auto unicode = adobe_glyph_unicode(name))
        return unicode;
    static const std::unordered_map<std::string_view, uint32_t> map = {{"space", 32},
                                                                       {"nbspace", 160},
                                                                       {"nonbreakingspace", 160},
                                                                       {"exclam", 33},
                                                                       {"quotedbl", 34},
                                                                       {"numbersign", 35},
                                                                       {"dollar", 36},
                                                                       {"percent", 37},
                                                                       {"ampersand", 38},
                                                                       {"quotesingle", 39},
                                                                       {"parenleft", 40},
                                                                       {"parenright", 41},
                                                                       {"asterisk", 42},
                                                                       {"plus", 43},
                                                                       {"comma", 44},
                                                                       {"hyphen", 45},
                                                                       {"period", 46},
                                                                       {"slash", 47},
                                                                       {"zero", 48},
                                                                       {"one", 49},
                                                                       {"two", 50},
                                                                       {"three", 51},
                                                                       {"four", 52},
                                                                       {"five", 53},
                                                                       {"six", 54},
                                                                       {"seven", 55},
                                                                       {"eight", 56},
                                                                       {"nine", 57},
                                                                       {"colon", 58},
                                                                       {"semicolon", 59},
                                                                       {"less", 60},
                                                                       {"equal", 61},
                                                                       {"greater", 62},
                                                                       {"question", 63},
                                                                       {"at", 64},
                                                                       {"bracketleft", 91},
                                                                       {"backslash", 92},
                                                                       {"bracketright", 93},
                                                                       {"asciicircum", 94},
                                                                       {"underscore", 95},
                                                                       {"grave", 96},
                                                                       {"braceleft", 123},
                                                                       {"bar", 124},
                                                                       {"braceright", 125},
                                                                       {"asciitilde", 126},
                                                                       {"exclamdown", 161},
                                                                       {"cent", 162},
                                                                       {"sterling", 163},
                                                                       {"currency", 164},
                                                                       {"yen", 165},
                                                                       {"brokenbar", 166},
                                                                       {"section", 167},
                                                                       {"dieresis", 168},
                                                                       {"copyright", 169},
                                                                       {"ordfeminine", 170},
                                                                       {"guillemotleft", 171},
                                                                       {"logicalnot", 172},
                                                                       {"registered", 174},
                                                                       {"macron", 175},
                                                                       {"degree", 176},
                                                                       {"plusminus", 177},
                                                                       {"twosuperior", 178},
                                                                       {"threesuperior", 179},
                                                                       {"acute", 180},
                                                                       {"mu", 181},
                                                                       {"paragraph", 182},
                                                                       {"periodcentered", 183},
                                                                       {"cedilla", 184},
                                                                       {"onesuperior", 185},
                                                                       {"ordmasculine", 186},
                                                                       {"guillemotright", 187},
                                                                       {"onequarter", 188},
                                                                       {"onehalf", 189},
                                                                       {"threequarters", 190},
                                                                       {"questiondown", 191},
                                                                       {"AE", 198},
                                                                       {"Eth", 208},
                                                                       {"multiply", 215},
                                                                       {"Oslash", 216},
                                                                       {"Thorn", 222},
                                                                       {"germandbls", 223},
                                                                       {"ae", 230},
                                                                       {"eth", 240},
                                                                       {"divide", 247},
                                                                       {"oslash", 248},
                                                                       {"thorn", 254},
                                                                       {"dotlessi", 305},
                                                                       {"Lslash", 321},
                                                                       {"lslash", 322},
                                                                       {"OE", 338},
                                                                       {"oe", 339},
                                                                       {"florin", 402},
                                                                       {"circumflex", 710},
                                                                       {"caron", 711},
                                                                       {"breve", 728},
                                                                       {"dotaccent", 729},
                                                                       {"ring", 730},
                                                                       {"ogonek", 731},
                                                                       {"tilde", 732},
                                                                       {"hungarumlaut", 733},
                                                                       {"endash", 0x2013},
                                                                       {"emdash", 0x2014},
                                                                       {"quoteleft", 0x2018},
                                                                       {"quoteright", 0x2019},
                                                                       {"quotesinglbase", 0x201a},
                                                                       {"quotedblleft", 0x201c},
                                                                       {"quotedblright", 0x201d},
                                                                       {"quotedblbase", 0x201e},
                                                                       {"dagger", 0x2020},
                                                                       {"daggerdbl", 0x2021},
                                                                       {"bullet", 0x2022},
                                                                       {"ellipsis", 0x2026},
                                                                       {"perthousand", 0x2030},
                                                                       {"guilsinglleft", 0x2039},
                                                                       {"guilsinglright", 0x203a},
                                                                       {"fraction", 0x2044},
                                                                       {"Euro", 0x20ac},
                                                                       {"trademark", 0x2122},
                                                                       {"minus", 0x2212},
                                                                       {"fi", 0xfb01},
                                                                       {"fl", 0xfb02},
                                                                       {"ff", 0xfb00},
                                                                       {"ffi", 0xfb03},
                                                                       {"ffl", 0xfb04},
                                                                       {"Alpha", 0x391},
                                                                       {"Beta", 0x392},
                                                                       {"Gamma", 0x393},
                                                                       {"Delta", 0x394},
                                                                       {"Epsilon", 0x395},
                                                                       {"Zeta", 0x396},
                                                                       {"Eta", 0x397},
                                                                       {"Theta", 0x398},
                                                                       {"Iota", 0x399},
                                                                       {"Kappa", 0x39a},
                                                                       {"Lambda", 0x39b},
                                                                       {"Mu", 0x39c},
                                                                       {"Nu", 0x39d},
                                                                       {"Xi", 0x39e},
                                                                       {"Omicron", 0x39f},
                                                                       {"Pi", 0x3a0},
                                                                       {"Rho", 0x3a1},
                                                                       {"Sigma", 0x3a3},
                                                                       {"Tau", 0x3a4},
                                                                       {"Upsilon", 0x3a5},
                                                                       {"Phi", 0x3a6},
                                                                       {"Chi", 0x3a7},
                                                                       {"Psi", 0x3a8},
                                                                       {"Omega", 0x3a9},
                                                                       {"alpha", 0x3b1},
                                                                       {"beta", 0x3b2},
                                                                       {"gamma", 0x3b3},
                                                                       {"delta", 0x3b4},
                                                                       {"epsilon", 0x3b5},
                                                                       {"zeta", 0x3b6},
                                                                       {"eta", 0x3b7},
                                                                       {"theta", 0x3b8},
                                                                       {"iota", 0x3b9},
                                                                       {"kappa", 0x3ba},
                                                                       {"lambda", 0x3bb},
                                                                       {"nu", 0x3bd},
                                                                       {"xi", 0x3be},
                                                                       {"omicron", 0x3bf},
                                                                       {"pi", 0x3c0},
                                                                       {"rho", 0x3c1},
                                                                       {"sigma", 0x3c3},
                                                                       {"tau", 0x3c4},
                                                                       {"upsilon", 0x3c5},
                                                                       {"phi", 0x3c6},
                                                                       {"chi", 0x3c7},
                                                                       {"psi", 0x3c8},
                                                                       {"omega", 0x3c9},
                                                                       {"integral", 0x222b},
                                                                       {"summation", 0x2211},
                                                                       {"product", 0x220f},
                                                                       {"infinity", 0x221e},
                                                                       {"partialdiff", 0x2202},
                                                                       {"radical", 0x221a},
                                                                       {"notequal", 0x2260},
                                                                       {"lessequal", 0x2264},
                                                                       {"greaterequal", 0x2265},
                                                                       {"approxequal", 0x2248},
                                                                       {"arrowleft", 0x2190},
                                                                       {"arrowup", 0x2191},
                                                                       {"arrowright", 0x2192},
                                                                       {"arrowdown", 0x2193},
                                                                       {"lozenge", 0x25ca},
                                                                       {"universal", 0x2200},
                                                                       {"existential", 0x2203},
                                                                       {"element", 0x2208},
                                                                       {"emptyset", 0x2205}};
    auto found = map.find(name);
    if (found != map.end())
        return found->second;
    static const std::pair<std::string_view, uint32_t> accents[] = {
        {"acute", 0x301},       {"grave", 0x300},  {"circumflex", 0x302}, {"tilde", 0x303},
        {"dieresis", 0x308},    {"ring", 0x30a},   {"cedilla", 0x327},    {"caron", 0x30c},
        {"breve", 0x306},       {"macron", 0x304}, {"ogonek", 0x328},     {"dotaccent", 0x307},
        {"hungarumlaut", 0x30b}};
    for (auto [suffix, mark] : accents)
        if (name.size() == suffix.size() + 1 && name.substr(1) == suffix) {
            wchar_t input[2] = {wchar_t(name[0]), wchar_t(mark)}, output[4]{};
            int n = NormalizeString(NormalizationC, input, 2, output, 4);
            if (n == 1)
                return output[0];
        }
    return 0;
}
static std::string name_for(uint32_t unicode) {
    static const auto names = [] {
        std::unordered_map<uint32_t, std::string_view> result;
        for (auto& name : standard_names())
            if (auto code = glyph_unicode(name))
                result.try_emplace(code, name);
        return result;
    }();
    if (auto found = names.find(unicode); found != names.end())
        return std::string(found->second);
    if (unicode >= 33 && unicode <= 126)
        return std::string(1, char(unicode));
    return {};
}
static void base_encoding(Font& f, std::string name) {
    for (unsigned i = 0; i < 256; ++i) {
        f.encoding[i] = i;
        f.glyph_names[i] = name_for(i);
    }
    if (name == "MacRomanEncoding" || name == "WinAnsiEncoding") {
        for (unsigned i = 0; i < 256; ++i) {
            char c = char(i);
            wchar_t w = 0;
            if (MultiByteToWideChar(name == "MacRomanEncoding" ? 10000 : 1252, 0, &c, 1, &w, 1))
                f.encoding[i] = w;
            f.glyph_names[i] = name_for(f.encoding[i]);
        }
    } else {
        f.encoding.fill(0);
        for (unsigned i = 32; i <= 126; ++i) {
            f.glyph_names[i] = standard_names()[i - 31];
            f.encoding[i] = glyph_unicode(f.glyph_names[i]);
        }
        static const std::pair<unsigned, unsigned> extended[] = {
            {161, 96},  {162, 97},  {163, 98},  {164, 99},  {165, 100}, {166, 101}, {167, 102}, {168, 103},
            {169, 104}, {170, 105}, {171, 106}, {172, 107}, {173, 108}, {174, 109}, {175, 110}, {177, 111},
            {178, 112}, {179, 113}, {180, 114}, {182, 115}, {183, 116}, {184, 117}, {185, 118}, {186, 119},
            {187, 120}, {188, 121}, {189, 122}, {191, 123}, {193, 124}, {194, 125}, {195, 126}, {196, 127},
            {197, 128}, {198, 129}, {199, 130}, {200, 131}, {202, 132}, {203, 133}, {205, 134}, {206, 135},
            {207, 136}, {208, 137}, {225, 138}, {227, 139}, {232, 140}, {233, 141}, {234, 142}, {235, 143},
            {241, 144}, {245, 145}, {248, 146}, {249, 147}, {250, 148}, {251, 149}};
        for (auto [code, sid] : extended) {
            f.glyph_names[code] = standard_names()[sid];
            f.encoding[code] = glyph_unicode(f.glyph_names[code]);
        }
    }
    if (f.base_name.find("Symbol") != std::string::npos) {
        for (auto entry : symbol_encoding) {
            f.glyph_names[entry.code] = entry.name;
            f.encoding[entry.code] = glyph_unicode(entry.name);
        }
        f.symbolic = true;
    }
    if (f.base_name.find("ZapfDingbats") != std::string::npos) {
        f.encoding.fill(0);
        for (auto entry : dingbats_encoding) {
            f.glyph_names[entry.code] = entry.name;
            f.encoding[entry.code] = glyph_unicode(entry.name);
        }
    }
}
static uint32_t be(View b, size_t p, unsigned n) {
    require(p <= b.size() && n <= b.size() - p, "Truncated font data");
    uint32_t r = 0;
    for (unsigned i = 0; i < n; ++i)
        r = (r << 8) | b[p + i];
    return r;
}
static std::vector<View> cff_index(View data, size_t& p) {
    uint32_t count = be(data, p, 2);
    p += 2;
    std::vector<View> out;
    if (!count)
        return out;
    unsigned size = be(data, p++, 1);
    require(size >= 1 && size <= 4, "Invalid CFF offset size");
    size_t base = p + (count + 1) * size;
    require(base <= data.size(), "Truncated CFF INDEX");
    uint32_t last = be(data, p, size);
    require(last == 1, "Invalid CFF INDEX start");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t next = be(data, p + (i + 1) * size, size);
        require(next >= last && next - 1 <= data.size() - base, "Invalid CFF INDEX offset");
        out.push_back(data.subspan(base + last - 1, next - last));
        last = next;
    }
    p = base + last - 1;
    return out;
}
static void cff_metadata(Font& f, bool native_encoding) {
    View data = f.program;
    require(data.size() >= 4 && data[0] == 1, "Unsupported CFF header");
    size_t p = data[2];
    auto names = cff_index(data, p);
    auto top = cff_index(data, p);
    auto strings = cff_index(data, p);
    require(!top.empty(), "Missing CFF top dictionary");
    std::unordered_map<unsigned, std::vector<double>> dict;
    std::vector<double> args;
    auto t = top[0];
    for (size_t i = 0; i < t.size();) {
        unsigned b = t[i++];
        if (b >= 32 && b <= 246)
            args.push_back(int(b) - 139);
        else if (b >= 247 && b <= 250) {
            args.push_back((b - 247) * 256 + be(t, i++, 1) + 108);
        } else if (b >= 251 && b <= 254) {
            args.push_back(-double((b - 251) * 256 + be(t, i++, 1) + 108));
        } else if (b == 28) {
            args.push_back(int16_t(be(t, i, 2)));
            i += 2;
        } else if (b == 29) {
            args.push_back(int32_t(be(t, i, 4)));
            i += 4;
        } else if (b == 30) {
            std::string s;
            bool done = false;
            while (i < t.size() && !done) {
                unsigned pair = t[i++];
                for (int k = 1; k >= 0; --k) {
                    unsigned nib = (pair >> (k * 4)) & 15;
                    if (nib == 15) {
                        done = true;
                        break;
                    }
                    if (nib <= 9)
                        s.push_back(char('0' + nib));
                    else if (nib == 10)
                        s.push_back('.');
                    else if (nib == 11)
                        s += 'E';
                    else if (nib == 12)
                        s += "E-";
                    else if (nib == 14)
                        s += '-';
                }
            }
            double n = 0;
            std::from_chars(s.data(), s.data() + s.size(), n);
            args.push_back(n);
        } else {
            unsigned op = b == 12 ? 1200 + be(t, i++, 1) : b;
            dict[op] = args;
            args.clear();
        }
        require(args.size() < 100, "CFF dictionary operand limit");
    }
    auto field = [&](unsigned op, int fallback = 0) {
        auto it = dict.find(op);
        return it != dict.end() && !it->second.empty() ? int(it->second[0]) : fallback;
    };
    int chars = field(17, -1), charset = field(15);
    require(chars >= 0 && size_t(chars) < data.size(), "Invalid CFF CharStrings offset");
    p = size_t(chars);
    auto glyphs = cff_index(data, p);
    bool cid = dict.contains(1230);
    auto sid_name = [&](unsigned sid) -> std::string {
        if (sid < standard_names().size())
            return standard_names()[sid];
        if (sid >= 391 && sid - 391 < strings.size())
            return string(strings[sid - 391]);
        return {};
    };
    std::vector<unsigned> ids(glyphs.size());
    if (charset == 0) {
        for (unsigned i = 0; i < ids.size(); ++i)
            ids[i] = i;
    } else if (charset > 2) {
        p = size_t(charset);
        unsigned format = be(data, p++, 1);
        require(format <= 2, "Invalid CFF charset format");
        size_t i = 1;
        while (i < ids.size()) {
            unsigned first = be(data, p, 2);
            p += 2;
            unsigned count = format == 0 ? 0 : be(data, p, format);
            p += format;
            require(count < ids.size() - i, "CFF charset range overflow");
            for (unsigned j = 0; j <= count; ++j)
                ids[i++] = first + j;
        }
    } else {
        for (unsigned i = 0; i < ids.size(); ++i)
            ids[i] = i;
    }
    for (unsigned i = 0; i < ids.size(); ++i) {
        if (cid)
            f.cff_cids[ids[i]] = uint16_t(i);
        else {
            auto name = sid_name(ids[i]);
            if (!name.empty())
                f.cff_names[name] = uint16_t(i);
        }
    }
    if (!cid && native_encoding) {
        int enc = field(16);
        if (enc > 1) {
            p = size_t(enc);
            unsigned format = be(data, p++, 1), count = be(data, p++, 1), gid = 1;
            if ((format & 127) == 0) {
                for (unsigned i = 0; i < count; ++i) {
                    unsigned code = be(data, p++, 1);
                    if (gid < ids.size()) {
                        f.glyph_names[code] = sid_name(ids[gid++]);
                        f.encoding[code] = glyph_unicode(f.glyph_names[code]);
                    }
                }
            } else if ((format & 127) == 1) {
                for (unsigned i = 0; i < count; ++i) {
                    unsigned first = be(data, p++, 1), left = be(data, p++, 1);
                    for (unsigned j = 0; j <= left; ++j) {
                        require(first + j < 256 && gid < ids.size(), "Invalid CFF encoding range");
                        f.glyph_names[first + j] = sid_name(ids[gid++]);
                        f.encoding[first + j] = glyph_unicode(f.glyph_names[first + j]);
                    }
                }
            }
            if (format & 128) {
                count = be(data, p++, 1);
                for (unsigned i = 0; i < count; ++i) {
                    unsigned code = be(data, p++, 1), sid = be(data, p, 2);
                    p += 2;
                    f.glyph_names[code] = sid_name(sid);
                    f.encoding[code] = glyph_unicode(f.glyph_names[code]);
                }
            }
        }
    }
}
static Bytes type1_decrypt(View data, uint16_t seed, int discard) {
    Bytes out;
    out.reserve(data.size());
    for (auto c : data) {
        uint8_t value = c ^ uint8_t(seed >> 8);
        seed = uint16_t((unsigned(c) + seed) * 52845 + 22719);
        if (discard > 0)
            --discard;
        else
            out.push_back(value);
    }
    return out;
}
static std::shared_ptr<Type1Program> type1_program(View raw, const Value& file) {
    auto owned = std::make_shared<Type1Program>();
    auto& f = *owned;
    Bytes flattened;
    if (raw.size() > 6 && raw[0] == 128) {
        size_t pos = 0;
        while (pos + 2 <= raw.size() && raw[pos] == 128) {
            unsigned type = raw[pos + 1];
            pos += 2;
            if (type == 3)
                break;
            require(pos + 4 <= raw.size(), "Truncated PFB font");
            uint32_t length = raw[pos] | uint32_t(raw[pos + 1]) << 8 | uint32_t(raw[pos + 2]) << 16 |
                              uint32_t(raw[pos + 3]) << 24;
            pos += 4;
            require(length <= raw.size() - pos, "Invalid PFB section");
            flattened.insert(flattened.end(), raw.begin() + pos, raw.begin() + pos + length);
            pos += length;
        }
        raw = flattened;
    }
    std::string all = string(raw);
    size_t ee = all.find("eexec");
    require(ee != std::string::npos, "Type 1 font missing eexec");
    size_t start = ee + 5;
    if (start < raw.size() && raw[start] == '\r')
        ++start;
    if (start < raw.size() && raw[start] == '\n')
        ++start;
    if (start < raw.size() && raw[start] == ' ')
        ++start;
    int declared = integer(get(file, "Length1"));
    if (declared > 0 && size_t(declared) >= start && size_t(declared) < raw.size() && flattened.empty())
        start = size_t(declared);
    auto encrypted = raw.subspan(start);
    Bytes ascii;
    bool is_hex = encrypted.size() >= 8;
    for (size_t i = 0; i < std::min<size_t>(8, encrypted.size()); ++i)
        if (!std::isxdigit(encrypted[i]) && !whitespace(encrypted[i]))
            is_hex = false;
    if (is_hex) {
        size_t len = 0;
        while (len < encrypted.size() && (std::isxdigit(encrypted[len]) || whitespace(encrypted[len])))
            ++len;
        ascii = ascii_hex(encrypted.first(len));
        encrypted = ascii;
    }
    Bytes plain = type1_decrypt(encrypted, 55665, 4);
    Parser p{plain, 0, 0, false};
    int leniv = 4;
    bool subrs = false, chars = false;
    std::vector<Value> operands;
    while (p.pos < p.data.size()) {
        skip_space(p);
        if (p.pos >= p.data.size())
            break;
        size_t old = p.pos;
        Value v;
        try {
            if (p.data[p.pos] == '{' || p.data[p.pos] == '}') {
                ++p.pos;
                operands.clear();
                continue;
            }
            v = parse(p);
        } catch (const std::exception&) {
            p.pos = old + 1;
            operands.clear();
            continue;
        }
        if (v.kind == Kind::name && v.text == "lenIV") {
            leniv = integer(parse(p), 4);
            continue;
        }
        if (v.kind == Kind::name && v.text == "Subrs") {
            subrs = true;
            chars = false;
            int n = integer(parse(p));
            require(n >= 0 && n <= 65536, "Type 1 Subrs limit");
            f.type1_subrs.resize(n);
            operands.clear();
            continue;
        }
        if (v.kind == Kind::name && v.text == "CharStrings") {
            chars = true;
            subrs = false;
            operands.clear();
            continue;
        }
        if (v.kind == Kind::keyword && (v.text == "RD" || v.text == "-|")) {
            require(operands.size() >= 2, "Invalid Type 1 binary string");
            auto size = integer(operands.back(), -1);
            auto key = operands[operands.size() - 2];
            if (p.pos < p.data.size() && whitespace(p.data[p.pos]))
                ++p.pos;
            require(size >= 0 && size_t(size) <= p.data.size() - p.pos, "Truncated Type 1 charstring");
            auto bytes = p.data.subspan(p.pos, size);
            p.pos += size;
            Bytes decoded = leniv < 0 ? Bytes(bytes.begin(), bytes.end()) : type1_decrypt(bytes, 4330, leniv);
            if (chars && key.kind == Kind::name)
                f.type1_chars[key.text] = std::move(decoded);
            else if (subrs && key.kind == Kind::number) {
                int n = integer(key, -1);
                if (n >= 0 && size_t(n) < f.type1_subrs.size())
                    f.type1_subrs[n] = std::move(decoded);
            }
            operands.clear();
        } else {
            operands.push_back(std::move(v));
            if (operands.size() > 8)
                operands.erase(operands.begin());
        }
    }
    {
        Parser enc{raw.first(ee), 0, 0, false};
        std::vector<Value> stack;
        while (enc.pos < enc.data.size()) {
            size_t old = enc.pos;
            try {
                Value v = parse(enc);
                if (v.kind == Kind::keyword && v.text == "put" && stack.size() >= 2) {
                    auto& name = stack.back();
                    auto& code = stack[stack.size() - 2];
                    int n = integer(code, -1);
                    if (n >= 0 && n < 256 && name.kind == Kind::name) {
                        f.glyph_names[n] = name.text;
                    }
                    stack.clear();
                } else {
                    stack.push_back(std::move(v));
                    if (stack.size() > 4)
                        stack.erase(stack.begin());
                }
            } catch (const std::exception&) {
                enc.pos = old + 1;
            }
        }
    }
    require(!f.type1_chars.empty(), "No Type 1 character programs");
    return owned;
}
struct T1State {
    Point position;
    std::vector<double> stack, results;
    std::vector<Segment> path;
    size_t budget = 100000;
    bool flex = false;
    std::vector<Point> flex_points;
};
static void type1_run(const Type1Program& font, View data, T1State& s, int depth) {
    require(depth < 32, "Type 1 subroutine depth limit");
    auto& a = s.stack;
    auto move = [&](double x, double y) {
        s.position.x += x;
        s.position.y += y;
        if (s.flex)
            s.flex_points.push_back(s.position);
        else
            s.path.push_back({0, s.position, {}, {}});
    };
    auto line = [&](double x, double y) {
        s.position.x += x;
        s.position.y += y;
        s.path.push_back({1, s.position, {}, {}});
    };
    auto curve = [&](double x1, double y1, double x2, double y2, double x3, double y3) {
        Point p1{s.position.x + x1, s.position.y + y1}, p2{p1.x + x2, p1.y + y2}, p3{p2.x + x3, p2.y + y3};
        s.path.push_back({2, p1, p2, p3});
        s.position = p3;
    };
    auto need = [&](size_t n) { require(a.size() >= n, "Type 1 operand stack underflow"); };
    for (size_t p = 0; p < data.size();) {
        require(s.budget-- > 0 && a.size() < 100, "Type 1 program limit");
        unsigned b = data[p++];
        if (b >= 32 && b <= 246) {
            a.push_back(int(b) - 139);
            continue;
        }
        if (b >= 247 && b <= 250) {
            a.push_back((b - 247) * 256 + be(data, p++, 1) + 108);
            continue;
        }
        if (b >= 251 && b <= 254) {
            a.push_back(-double((b - 251) * 256 + be(data, p++, 1) + 108));
            continue;
        }
        if (b == 255) {
            a.push_back(int32_t(be(data, p, 4)));
            p += 4;
            continue;
        }
        unsigned op = b == 12 ? 1200 + be(data, p++, 1) : b;
        if (op == 10) {
            need(1);
            int index = int(a.back());
            a.pop_back();
            require(index >= 0 && size_t(index) < font.type1_subrs.size(), "Invalid Type 1 subroutine");
            type1_run(font, font.type1_subrs[index], s, depth + 1);
            continue;
        }
        if (op == 11)
            return;
        if (op == 1212) {
            need(2);
            double rhs = a.back();
            a.pop_back();
            require(rhs != 0, "Type 1 division by zero");
            a.back() /= rhs;
            continue;
        }
        if (op == 1217) {
            require(!s.results.empty(), "Type 1 othersubr result missing");
            a.push_back(s.results.back());
            s.results.pop_back();
            continue;
        }
        if (op == 1216) {
            need(2);
            int sub = int(a.back());
            a.pop_back();
            int count = int(a.back());
            a.pop_back();
            require(count >= 0 && size_t(count) <= a.size(), "Invalid Type 1 othersubr");
            if (sub == 1) {
                s.flex = true;
                s.flex_points.clear();
                s.flex_points.push_back(s.position);
            } else if (sub == 0) {
                s.flex = false;
                auto& pnts = s.flex_points;
                if (pnts.size() >= 7) {
                    size_t start = pnts.size() - 6;
                    s.path.push_back({2, pnts[start], pnts[start + 1], pnts[start + 2]});
                    s.path.push_back({2, pnts[start + 3], pnts[start + 4], pnts[start + 5]});
                }
                s.results = {s.position.y, s.position.x};
            } else if (sub == 3 && count)
                s.results.push_back(a[a.size() - count]);
            a.resize(a.size() - count);
            continue;
        }
        if (op == 13) {
            need(2);
            s.position = {a[0], 0};
        } else if (op == 1207) {
            need(4);
            s.position = {a[0], a[1]};
        } else if (op == 21) {
            need(2);
            move(a[0], a[1]);
        } else if (op == 22) {
            need(1);
            move(a[0], 0);
        } else if (op == 4) {
            need(1);
            move(0, a[0]);
        } else if (op == 5) {
            for (size_t i = 0; i + 1 < a.size(); i += 2)
                line(a[i], a[i + 1]);
        } else if (op == 6) {
            for (double n : a)
                line(n, 0);
        } else if (op == 7) {
            for (double n : a)
                line(0, n);
        } else if (op == 8) {
            for (size_t i = 0; i + 5 < a.size(); i += 6)
                curve(a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
        } else if (op == 30) {
            need(4);
            curve(0, a[0], a[1], a[2], a[3], 0);
        } else if (op == 31) {
            need(4);
            curve(a[0], 0, a[1], a[2], 0, a[3]);
        } else if (op == 9)
            s.path.push_back({3, {}, {}, {}});
        else if (op == 1233) {
            need(2);
            s.position = {a[0], a[1]};
        } else if (op == 1206) {
            need(5);
            auto args = a;
            a.clear();
            Font encoding;
            base_encoding(encoding, "StandardEncoding");
            int base = int(args[3]), accent = int(args[4]);
            if (base >= 0 && base < 256) {
                auto it = font.type1_chars.find(encoding.glyph_names[base]);
                if (it != font.type1_chars.end())
                    type1_run(font, it->second, s, depth + 1);
            }
            if (accent >= 0 && accent < 256) {
                auto it = font.type1_chars.find(encoding.glyph_names[accent]);
                if (it != font.type1_chars.end()) {
                    T1State part;
                    part.budget = s.budget;
                    type1_run(font, it->second, part, depth + 1);
                    s.budget = part.budget;
                    for (auto seg : part.path) {
                        for (auto point : {&seg.p1, &seg.p2, &seg.p3}) {
                            point->x += args[1] - args[0];
                            point->y += args[2];
                        }
                        s.path.push_back(seg);
                    }
                }
            }
            return;
        } else if (op == 14) {
            a.clear();
            return;
        }
        a.clear();
    }
}
std::vector<Segment> type1_outline(Font& font, std::string_view name) {
    if (!font.type1_program)
        return {};
    auto& program = *font.type1_program;
    auto it = program.type1_chars.find(std::string(name));
    if (it == program.type1_chars.end())
        return {};
    T1State s;
    type1_run(program, it->second, s, 0);
    return s.path;
}
static IDWriteFontFace* system_face(Renderer& r, const Font& f) {
    auto name = f.base_name;
    if (name.size() > 7 && name[6] == '+')
        name.erase(0, 7);
    std::wstring family = utf8(name);
    if (name.find("Helvetica") != std::string::npos || name.find("Arial") != std::string::npos)
        family = L"Arial";
    else if (name.find("Times") != std::string::npos)
        family = L"Times New Roman";
    else if (name.find("Courier") != std::string::npos)
        family = L"Courier New";
    else if (name == "Symbol")
        family = L"Symbol";
    else if (name.find("Dingbats") != std::string::npos)
        family = L"Segoe UI Symbol";
    bool bold = name.find("Bold") != std::string::npos,
         italic = name.find("Italic") != std::string::npos || name.find("Oblique") != std::string::npos;
    IDWriteFontCollection* collection = nullptr;
    IDWriteFontFamily* fam = nullptr;
    IDWriteFont* font = nullptr;
    IDWriteFontFace* face = nullptr;
    try {
        hr(r.write->GetSystemFontCollection(&collection), "Get system fonts");
        UINT32 index = 0;
        BOOL exists = FALSE;
        collection->FindFamilyName(family.c_str(), &index, &exists);
        if (!exists)
            collection->FindFamilyName(f.composite ? L"Microsoft YaHei" : L"Arial", &index, &exists);
        hr(collection->GetFontFamily(index, &fam), "Get fallback font family");
        hr(fam->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL,
                                     italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font),
           "Find matching font");
        hr(font->CreateFontFace(&face), "Create system font face");
    } catch (...) {
        release(font);
        release(fam);
        release(collection);
        throw;
    }
    release(font);
    release(fam);
    release(collection);
    return face;
}
static void reverse_font_cmap(Font& font) {
    IDWriteFontFace1* face = nullptr;
    if (!font.face || FAILED(font.face->QueryInterface(IID_PPV_ARGS(&face))))
        return;
    UINT32 count = 0;
    face->GetUnicodeRanges(0, nullptr, &count);
    if (count > 4096) {
        release(face);
        return;
    }
    std::vector<DWRITE_UNICODE_RANGE> ranges(count);
    if (FAILED(face->GetUnicodeRanges(count, ranges.data(), &count))) {
        release(face);
        return;
    }
    std::array<UINT32, 1024> codes{};
    std::array<UINT16, 1024> glyphs{};
    size_t visited = 0;
    for (auto range : ranges) {
        uint32_t first = range.first, last = std::min<uint32_t>(range.last, 0x10ffff);
        while (first <= last && visited < 1'000'000) {
            UINT32 n = std::min<uint32_t>(1024, last - first + 1);
            for (UINT32 i = 0; i < n; ++i)
                codes[i] = first + i;
            if (SUCCEEDED(face->GetGlyphIndices(codes.data(), n, glyphs.data())))
                for (UINT32 i = 0; i < n; ++i)
                    if (glyphs[i] && !font.glyph_to_unicode.contains(glyphs[i]))
                        font.glyph_to_unicode[glyphs[i]] = codes[i];
            first += n;
            visited += n;
        }
    }
    release(face);
}
Font& load_font(Renderer& r, Document& d, const Value& input) {
    ProfileTimer timer(r.profiling ? &r.profile.font_load_ms : nullptr);
    const auto& dictionary = resolve(d, input);
    auto found = r.fonts.find(&dictionary);
    if (found != r.fonts.end())
        return *found->second;
    auto owned = std::make_unique<Font>();
    Font& f = *owned;
    f.dictionary = dictionary;
    f.base_name = get(dictionary, "BaseFont").text;
    f.composite = named(get(dictionary, "Subtype"), "Type0");
    f.type3 = named(get(dictionary, "Subtype"), "Type3");
    f.descendant = f.composite ? resolve(d, at(entry(d, dictionary, "DescendantFonts"), 0)) : dictionary;
    auto& descriptor = entry(d, f.descendant, "FontDescriptor");
    f.symbolic = (integer(get(descriptor, "Flags")) & 4) != 0;
    f.default_width = num(get(descriptor, "MissingWidth"));
    f.ascent = integer(get(descriptor, "Ascent"), 800);
    f.descent = integer(get(descriptor, "Descent"), -200);
    auto& encoding = entry(d, dictionary, "Encoding");
    auto encoding_name = encoding.kind == Kind::name ? encoding.text : get(encoding, "BaseEncoding").text;
    bool native_encoding = encoding.kind == Kind::null ||
                           (encoding.kind == Kind::dict && get(encoding, "BaseEncoding").kind == Kind::null);
    base_encoding(f, encoding_name);
    auto& unicode = entry(d, dictionary, "ToUnicode");
    if (unicode.has_stream) {
        try {
            auto bytes = decode_stream(d, unicode).bytes;
            parse_cmap(bytes, f.to_unicode, true);
        } catch (const std::exception& e) {
            warn(d, std::string("Invalid ToUnicode CMap: ") + e.what());
        }
    }
    if (f.composite) {
        if (encoding.has_stream) {
            auto bytes = decode_stream(d, encoding).bytes;
            parse_cmap(bytes, f.cmap, false);
        } else if (encoding_name == "Identity-H" || encoding_name == "Identity-V") {
            f.cmap.identity = true;
            f.cmap.vertical = encoding_name.ends_with("-V");
            f.cmap.ranges.push_back({0, 65535, 2});
        } else
            require(builtin_cmap(encoding_name, f.cmap, false), "Unknown predefined CMap: " + encoding_name);
        auto& info = entry(d, f.descendant, "CIDSystemInfo");
        auto ordering = get(info, "Ordering").text;
        if (get(info, "Registry").text == "Adobe" && !ordering.empty() && ordering != "Identity") {
            std::string name = "Adobe-" + ordering + "-UCS2";
            auto cached_map = r.character_maps.find(name);
            if (cached_map != r.character_maps.end())
                f.cid_unicode = cached_map->second;
            else {
                auto map = std::make_shared<CMap>();
                if (builtin_cmap(name, *map, true)) {
                    f.cid_unicode = map;
                    r.character_maps[name] = std::move(map);
                }
            }
        }
        f.default_width = num(entry(d, f.descendant, "DW"), 1000);
        auto& widths = entry(d, f.descendant, "W");
        for (size_t i = 0; i < widths.array.size();) {
            int first = integer(widths.array[i++], -1);
            require(first >= 0 && first <= 0x10ffff && i < widths.array.size(), "Invalid CID width range");
            auto& next = resolve(d, widths.array[i++]);
            if (next.kind == Kind::array) {
                require(next.array.size() <= 1000000, "CID width count limit");
                for (size_t j = 0; j < next.array.size(); ++j)
                    f.widths[uint32_t(first + j)] = num(next.array[j]);
            } else {
                int last = integer(next, -1);
                require(last >= first && last - first <= 1000000 && i < widths.array.size(),
                        "Invalid CID width range");
                double w = num(widths.array[i++]);
                for (int j = first; j <= last; ++j)
                    f.widths[uint32_t(j)] = w;
            }
        }
        auto& vdefault = entry(d, f.descendant, "DW2");
        f.default_vorigin = num(at(vdefault, 0), 880);
        f.default_vertical = num(at(vdefault, 1), -1000);
        auto& vwidths = entry(d, f.descendant, "W2");
        for (size_t i = 0; i < vwidths.array.size();) {
            int first = integer(vwidths.array[i++], -1);
            require(first >= 0 && i < vwidths.array.size(), "Invalid vertical CID widths");
            auto& v = resolve(d, vwidths.array[i++]);
            if (v.kind == Kind::array) {
                for (size_t j = 0; j + 2 < v.array.size(); j += 3)
                    f.vertical_metrics[uint32_t(first + j / 3)] = {num(v.array[j]), num(v.array[j + 1]),
                                                                   num(v.array[j + 2])};
            } else {
                int last = integer(v, -1);
                require(last >= first && last - first <= 1000000 && i + 2 < vwidths.array.size(),
                        "Invalid vertical CID range");
                std::array<double, 3> metric{num(vwidths.array[i]), num(vwidths.array[i + 1]),
                                             num(vwidths.array[i + 2])};
                i += 3;
                for (int j = first; j <= last; ++j)
                    f.vertical_metrics[uint32_t(j)] = metric;
            }
        }
        auto& gid = entry(d, f.descendant, "CIDToGIDMap");
        if (gid.has_stream)
            f.cid_to_gid = decode_stream(d, gid).bytes;
    } else {
        int first = integer(get(dictionary, "FirstChar"));
        auto& widths = entry(d, dictionary, "Widths");
        for (size_t i = 0; i < widths.array.size() && i < 65536; ++i)
            f.widths[uint32_t(first + int(i))] = num(widths.array[i]);
    }
    if (f.type3) {
        auto& m = entry(d, dictionary, "FontMatrix");
        if (m.array.size() == 6)
            f.matrix = {num(at(m, 0)), num(at(m, 1)), num(at(m, 2)),
                        num(at(m, 3)), num(at(m, 4)), num(at(m, 5))};
    } else {
        const Value* program = &entry(d, descriptor, "FontFile2");
        bool cff = false, type1 = false;
        if (!program->has_stream) {
            program = &entry(d, descriptor, "FontFile3");
            cff =
                named(get(*program, "Subtype"), "Type1C") || named(get(*program, "Subtype"), "CIDFontType0C");
        }
        if (!program->has_stream) {
            program = &entry(d, descriptor, "FontFile");
            type1 = program->has_stream;
        }
        if (program->has_stream) {
            IDWriteFontFile* file = nullptr;
            try {
                if (type1) {
                    auto cached = r.type1_programs.find(program);
                    if (cached != r.type1_programs.end())
                        f.type1_program = cached->second;
                    else {
                        auto bytes = decode_stream(d, *program).bytes;
                        f.type1_program = type1_program(bytes, *program);
                        r.type1_programs.emplace(program, f.type1_program);
                    }
                    // Font dictionaries can share one program but have different
                    // encodings and widths. Only the decoded program is shared.
                    if (native_encoding)
                        for (size_t i = 0; i < f.glyph_names.size(); ++i) {
                            auto& name = f.type1_program->glyph_names[i];
                            if (!name.empty()) {
                                f.glyph_names[i] = name;
                                f.encoding[i] = glyph_unicode(name);
                            }
                        }
                    f.type1 = true;
                    f.embedded = true;
                } else {
                    f.program = decode_stream(d, *program).bytes;
                    if (cff)
                        cff_metadata(f, native_encoding);
                    hr(r.loader->CreateInMemoryFontFileReference(r.write, f.program.data(),
                                                                 UINT32(f.program.size()), nullptr, &file),
                       "Load embedded font file");
                    BOOL supported = FALSE;
                    DWRITE_FONT_FILE_TYPE file_type{};
                    DWRITE_FONT_FACE_TYPE face_type{};
                    UINT32 faces = 0;
                    if (cff)
                        face_type = DWRITE_FONT_FACE_TYPE_RAW_CFF;
                    else {
                        hr(file->Analyze(&supported, &file_type, &face_type, &faces),
                           "Analyze embedded font");
                        require(supported, "Unsupported embedded font format");
                    }
                    hr(r.write->CreateFontFace(face_type, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &f.face),
                       "Create embedded font face");
                    f.embedded = true;
                }
                release(file);
            } catch (const std::exception& e) {
                release(file);
                release(f.face);
                f.type1 = false;
                f.embedded = false;
                warn(d, "Font substitution for " + f.base_name + ": " + e.what());
            }
        }
        if (!f.face && !f.type1)
            f.face = system_face(r, f);
    }
    // Without BaseEncoding, Differences changes the font's own encoding.
    // Apply it after the embedded program has supplied that base mapping.
    if (!f.composite && encoding.kind == Kind::dict) {
        int code = 0;
        for (auto& v : entry(d, encoding, "Differences").array) {
            if (v.kind == Kind::number)
                code = integer(v);
            else if (v.kind == Kind::name) {
                if (code >= 0 && code < 256) {
                    f.glyph_names[code] = v.text;
                    f.encoding[code] = glyph_unicode(v.text);
                }
                ++code;
            }
        }
    }
    if (f.composite && f.to_unicode.unicode.empty() && !f.cid_unicode)
        reverse_font_cmap(f);
    r.fonts[&dictionary] = std::move(owned);
    return f;
}
static uint32_t first_unicode(std::wstring_view s) {
    if (s.empty())
        return 0;
    if (s.size() > 1 && s[0] >= 0xd800 && s[0] <= 0xdbff && s[1] >= 0xdc00 && s[1] <= 0xdfff)
        return 0x10000 + (uint32_t(s[0]) - 0xd800) * 1024 + uint32_t(s[1]) - 0xdc00;
    return s[0];
}
static std::wstring unicode_string(uint32_t c) {
    if (!c)
        return {};
    if (c <= 0xffff)
        return std::wstring(1, wchar_t(c));
    c -= 0x10000;
    return {wchar_t(0xd800 + (c >> 10)), wchar_t(0xdc00 + (c & 1023))};
}
std::vector<Glyph> decode_text(Font& f, std::string_view text) {
    std::vector<Glyph> out;
    out.reserve(text.size());
    for (size_t p = 0; p < text.size();) {
        Glyph g;
        unsigned bytes = 1;
        uint32_t code = uint8_t(text[p]);
        if (f.composite) {
            bool matched = false;
            for (unsigned n = 1; n <= 4 && p + n <= text.size(); ++n) {
                uint32_t candidate = big_code(text.substr(p, n));
                for (auto range : f.cmap.ranges)
                    if (range.bytes == n && candidate >= range.low && candidate <= range.high) {
                        code = candidate;
                        bytes = n;
                        matched = true;
                        break;
                    }
                if (matched)
                    break;
            }
            if (!matched) {
                bytes = unsigned(std::min<size_t>(2, text.size() - p));
                code = big_code(text.substr(p, bytes));
            }
        }
        p += bytes;
        g.code = code;
        g.cid = code;
        if (auto it = f.cmap.cid.find(code); it != f.cmap.cid.end())
            g.cid = it->second;
        auto tu = f.to_unicode.unicode.find(code);
        uint32_t unicode = f.composite ? 0 : f.encoding[code & 255];
        if (tu != f.to_unicode.unicode.end())
            g.text = tu->second;
        else if (f.cid_unicode && f.cid_unicode->unicode.contains(g.cid))
            g.text = f.cid_unicode->unicode.at(g.cid);
        else
            g.text = unicode_string(unicode);
        if (!unicode)
            unicode = first_unicode(g.text);
        if (!f.composite)
            g.name = f.glyph_names[code & 255];
        if (f.composite && f.embedded) {
            if (!f.cid_to_gid.empty()) {
                size_t off = size_t(g.cid) * 2;
                if (off + 1 < f.cid_to_gid.size())
                    g.index = uint16_t(f.cid_to_gid[off] * 256 + f.cid_to_gid[off + 1]);
            } else if (auto cid = f.cff_cids.find(g.cid); cid != f.cff_cids.end())
                g.index = cid->second;
            else
                g.index = uint16_t(g.cid);
        } else if (auto it = f.cff_names.find(g.name); it != f.cff_names.end())
            g.index = it->second;
        else if (f.face) {
            // A symbolic TrueType cmap is indexed by the PDF character code.
            // DirectWrite also aliases low Unicode values into that cmap, so
            // trying the glyph-name Unicode first can return a wrong glyph.
            if (f.embedded && f.symbolic && f.face->IsSymbolFont()) {
                uint32_t symbol = 0xf000 + (code & 255);
                f.face->GetGlyphIndices(&symbol, 1, &g.index);
                if (!g.index)
                    f.face->GetGlyphIndices(&code, 1, &g.index);
            }
            if (!g.index)
                f.face->GetGlyphIndices(&unicode, 1, &g.index);
            if (!g.index && f.symbolic) {
                uint32_t symbol = 0xf000 + (code & 255);
                f.face->GetGlyphIndices(&symbol, 1, &g.index);
                if (!g.index)
                    f.face->GetGlyphIndices(&code, 1, &g.index);
            }
        }
        if (g.text.empty()) {
            auto mapping = f.glyph_to_unicode.find(g.index);
            if (mapping != f.glyph_to_unicode.end())
                g.text = unicode_string(mapping->second);
        }
        auto w = f.widths.find(g.cid);
        g.advance = w == f.widths.end() ? f.default_width : w->second;
        if (w == f.widths.end() && g.advance == 0 && f.face) {
            DWRITE_GLYPH_METRICS metric{};
            DWRITE_FONT_METRICS fm{};
            f.face->GetMetrics(&fm);
            if (SUCCEEDED(f.face->GetDesignGlyphMetrics(&g.index, 1, &metric)) && fm.designUnitsPerEm)
                g.advance = double(metric.advanceWidth) * 1000 / fm.designUnitsPerEm;
        }
        // Type 3 widths use glyph space. Convert the horizontal component to
        // the 1000-unit text-space metrics used by the renderer.
        if (f.type3)
            g.advance *= f.matrix.a * 1000;
        g.vx = g.advance / 2;
        g.vy = f.default_vorigin;
        g.vadvance = f.default_vertical;
        if (auto v = f.vertical_metrics.find(g.cid); v != f.vertical_metrics.end()) {
            g.vadvance = v->second[0];
            g.vx = v->second[1];
            g.vy = v->second[2];
        }
        out.push_back(std::move(g));
    }
    return out;
}
} // namespace pdf
