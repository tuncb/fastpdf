#include "deployment.h"
#include "pdf.h"
#include <charconv>
#include <cctype>
#include <map>

namespace deployment {
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
Version parse_version(std::string_view text) {
    if (text.starts_with('v'))
        text.remove_prefix(1);
    Version version;
    unsigned* parts[] = {&version.major, &version.minor, &version.patch};
    for (int i = 0; i < 3; ++i) {
        const auto end = i == 2 ? text.size() : text.find('.');
        check(end != std::string_view::npos && end > 0, "Invalid release version");
        const auto part = text.substr(0, end);
        check(part.size() == 1 || part.front() != '0', "Invalid release version");
        const auto result = std::from_chars(part.data(), part.data() + part.size(), *parts[i]);
        check(result.ec == std::errc{} && result.ptr == part.data() + part.size() && *parts[i] <= 65535,
              "Invalid release version");
        text.remove_prefix(end + (i != 2 ? 1 : 0));
    }
    return version;
}
std::string version_string(Version version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.patch);
}
struct Json {
    enum Kind { null, string, object, array, boolean, number } kind = null;
    std::string text;
    bool flag = false;
    std::map<std::string, Json> properties;
    std::vector<Json> values;
};
struct JsonParser {
    std::string_view input;
    size_t position = 0;
};
static void whitespace(JsonParser& parser) {
    while (parser.position < parser.input.size() &&
           std::string_view(" \r\n\t").find(parser.input[parser.position]) != std::string_view::npos)
        ++parser.position;
}
static char take(JsonParser& parser) {
    check(parser.position < parser.input.size(), "Truncated release JSON");
    return parser.input[parser.position++];
}
static unsigned hex_quad(JsonParser& parser) {
    unsigned result = 0;
    for (int i = 0; i < 4; ++i) {
        const char digit = take(parser);
        const auto index =
            std::string_view("0123456789abcdef").find(char(std::tolower(static_cast<unsigned char>(digit))));
        check(index != std::string_view::npos, "Invalid JSON Unicode escape");
        result = result * 16 + unsigned(index);
    }
    return result;
}
static std::string json_string(JsonParser& parser) {
    check(take(parser) == '"', "Expected JSON string");
    std::string result;
    for (;;) {
        char c = take(parser);
        if (c == '"')
            return result;
        check(static_cast<unsigned char>(c) >= 0x20, "Invalid JSON string");
        if (c != '\\') {
            result += c;
            continue;
        }
        c = take(parser);
        switch (c) {
        case '"':
        case '\\':
        case '/':
            result += c;
            break;
        case 'b':
            result += '\b';
            break;
        case 'f':
            result += '\f';
            break;
        case 'n':
            result += '\n';
            break;
        case 'r':
            result += '\r';
            break;
        case 't':
            result += '\t';
            break;
        case 'u': {
            unsigned value = hex_quad(parser);
            std::wstring wide(1, wchar_t(value));
            if (value >= 0xd800 && value <= 0xdbff) {
                check(take(parser) == '\\' && take(parser) == 'u', "Invalid JSON surrogate pair");
                value = hex_quad(parser);
                check(value >= 0xdc00 && value <= 0xdfff, "Invalid JSON surrogate pair");
                wide += wchar_t(value);
            } else
                check(value < 0xdc00 || value > 0xdfff, "Invalid JSON surrogate pair");
            result += pdf::utf8(wide);
            break;
        }
        default:
            throw std::runtime_error("Invalid JSON escape");
        }
    }
}
static Json json_value(JsonParser& parser, unsigned depth = 0) {
    check(depth <= 32, "Release JSON is too deeply nested");
    whitespace(parser);
    check(parser.position < parser.input.size(), "Missing JSON value");
    Json value;
    char c = parser.input[parser.position];
    if (c == '"') {
        value.kind = Json::string;
        value.text = json_string(parser);
    } else if (c == '{' || c == '[') {
        ++parser.position;
        value.kind = c == '{' ? Json::object : Json::array;
        const char closing = c == '{' ? '}' : ']';
        whitespace(parser);
        if (parser.position < parser.input.size() && parser.input[parser.position] == closing) {
            ++parser.position;
            return value;
        }
        for (;;) {
            whitespace(parser);
            if (c == '{') {
                auto key = json_string(parser);
                whitespace(parser);
                check(take(parser) == ':', "Expected JSON colon");
                check(value.properties.emplace(std::move(key), json_value(parser, depth + 1)).second,
                      "Duplicate JSON property");
            } else
                value.values.push_back(json_value(parser, depth + 1));
            whitespace(parser);
            char separator = take(parser);
            if (separator == closing)
                break;
            check(separator == ',', "Expected JSON comma");
        }
    } else {
        const size_t begin = parser.position;
        while (parser.position < parser.input.size() &&
               std::string_view(" \r\n\t,]}").find(parser.input[parser.position]) == std::string_view::npos)
            ++parser.position;
        auto token = parser.input.substr(begin, parser.position - begin);
        if (token == "true" || token == "false") {
            value.kind = Json::boolean;
            value.flag = token == "true";
        } else if (token != "null") {
            double numeric = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), numeric);
            auto digits = token;
            if (digits.starts_with('-'))
                digits.remove_prefix(1);
            check(!digits.empty() && digits[0] >= '0' && digits[0] <= '9' &&
                      !(digits.size() > 1 && digits[0] == '0' && digits[1] >= '0' && digits[1] <= '9') &&
                      parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
                      std::isfinite(numeric),
                  "Invalid JSON value");
            value.kind = Json::number;
        }
    }
    return value;
}
static const Json& property(const Json& value, const char* key, Json::Kind kind) {
    check(value.kind == Json::object, "Expected release object");
    const auto found = value.properties.find(key);
    check(found != value.properties.end() && found->second.kind == kind,
          "Missing or invalid release property");
    return found->second;
}
Release parse_release(std::string_view json) {
    check(json.size() <= 2 * 1024 * 1024, "Release response is too large");
    JsonParser parser{json};
    const auto root = json_value(parser);
    whitespace(parser);
    check(parser.position == json.size(), "Unexpected data after release JSON");
    check(!property(root, "draft", Json::boolean).flag && !property(root, "prerelease", Json::boolean).flag,
          "Only stable published releases can be installed");
    Release release;
    release.tag = property(root, "tag_name", Json::string).text;
    release.version = parse_version(release.tag);
    check(release.tag == "v" + version_string(release.version), "Noncanonical release tag");
    for (const auto& asset : property(root, "assets", Json::array).values) {
        const auto& name = property(asset, "name", Json::string).text;
        if (name != "fastpdf.exe" && name != "SHA256SUMS.txt")
            continue;
        auto& destination = name == "fastpdf.exe" ? release.executable_url : release.checksum_url;
        check(destination.empty(), "Duplicate release asset");
        destination = property(asset, "browser_download_url", Json::string).text;
        check(destination == "https://github.com/tuncb/fastpdf/releases/download/" + release.tag + "/" + name,
              "Unexpected release download URL");
    }
    check(!release.executable_url.empty() && !release.checksum_url.empty(),
          "This release does not contain the standalone FastPDF executable and checksum");
    return release;
}
std::string parse_checksum(std::string_view text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    check(text.size() == 77 && text.substr(64) == "  fastpdf.exe", "Invalid fastpdf.exe checksum entry");
    std::string hash(text.substr(0, 64));
    for (char& c : hash) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
        check(std::string_view("0123456789abcdef").find(c) != std::string_view::npos,
              "Invalid SHA-256 checksum");
    }
    return hash;
}
std::wstring quote_argument(std::wstring_view argument) {
    check(argument.find(L'\0') == std::wstring_view::npos, "Invalid command-line argument");
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::vector<std::wstring> session_arguments(const Session& session) {
    return {L"--restore-view",
            std::to_wstring(session.page),
            std::to_wstring(session.rotation),
            std::to_wstring(session.fit),
            std::to_wstring(session.zoom),
            session.document};
}
Session parse_session(const std::vector<std::wstring>& arguments) {
    check(arguments.size() == 6 && arguments[0] == L"--restore-view", "Invalid restored session");
    auto number = [&](size_t index) {
        const auto text = pdf::utf8(arguments[index]);
        double result = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result),
              "Invalid restored view");
        return result;
    };
    const double page = number(1), rotation = number(2), fit = number(3), zoom = number(4);
    check(page >= 0 && page <= 10000000 && std::floor(page) == page &&
              (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) && fit >= 0 &&
              fit <= 2 && std::floor(fit) == fit && zoom >= 0.01 && zoom <= 100,
          "Invalid restored view");
    return {arguments[5], int(page), int(rotation), int(fit), zoom};
}
} // namespace deployment
