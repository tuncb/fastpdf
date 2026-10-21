#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
constexpr size_t max_stream = 256u * 1024u * 1024u;
constexpr uint32_t max_objects = 8'000'000;
constexpr size_t max_nodes = 2'000'000;
enum class Kind : uint8_t { null, boolean, number, name, string, array, dict, ref, keyword };
struct Value {
    Kind kind = Kind::null;
    double number = 0;
    uint32_t object = 0, generation = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> dict;
    View stream;
    bool has_stream = false;
};
struct Parser {
    View data;
    size_t pos = 0, nodes = 0;
    bool references = true;
};
struct Xref {
    uint64_t offset = 0;
    uint32_t index = 0, generation = 0;
    uint8_t type = 255;
};
struct Mapping {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    View bytes;
};
struct Security {
    bool enabled = false, metadata = true;
    int revision = 0, version = 0;
    std::string stream_method = "Identity", string_method = "Identity";
    Bytes key;
};
struct Rect {
    double x0 = 0, y0 = 0, x1 = 612, y1 = 792;
};
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
struct Point {
    double x = 0, y = 0;
};
struct Page {
    Value reference, resources, contents, annotations;
    Rect box;
    int rotation = 0;
    double user_unit = 1;
};
struct Outline {
    std::wstring title;
    int page = -1, depth = 0;
};
struct CalculatorProgram;
struct Document {
    Mapping file;
    Value trailer, catalog;
    std::vector<Xref> xref;
    std::unordered_map<uint32_t, std::unique_ptr<Value>> objects;
    std::unordered_set<uint32_t> resolving;
    std::unordered_set<uint32_t> object_streams;
    std::unordered_map<const uint8_t*, std::shared_ptr<Bytes>> function_samples;
    std::unordered_map<const uint8_t*, std::shared_ptr<CalculatorProgram>> calculators;
    size_t function_cache_bytes = 0;
    std::vector<Page> pages;
    std::vector<Outline> outline;
    std::unordered_map<std::string, const Value*> named_destinations;
    bool destinations_loaded = false;
    Security security;
    std::vector<std::string> warnings;
    std::wstring path;
    std::atomic<bool>* cancel = nullptr;
    size_t parsed_nodes = 0;
    bool repaired = false;
};
struct DecodeResult {
    Bytes bytes;
    std::string image_filter;
    Value params;
};

[[noreturn]] void fail(std::string_view message);
void require(bool condition, std::string_view message);
bool whitespace(uint8_t c);
bool delimiter(uint8_t c);
void skip_space(Parser& p);
std::string_view token(Parser& p);
Value parse(Parser& p, int depth = 0);
const Value& get(const Value& value, std::string_view key);
double num(const Value& value, double fallback = 0);
int integer(const Value& value, int fallback = 0);
bool boolean(const Value& value, bool fallback = false);
bool named(const Value& value, std::string_view name);
const Value& at(const Value& value, size_t index);
Value reference(uint32_t id, uint32_t generation = 0);
View view(std::string_view text);
std::string string(View bytes);
std::wstring utf8(std::string_view text);
std::wstring pdf_text(std::string_view text);
std::string utf8(std::wstring_view text);
Matrix multiply(Matrix outer, Matrix inner);
Point transform(Matrix matrix, Point point);
Matrix inverse(Matrix matrix);
Rect rectangle(const Value& value, Rect fallback = {});
void map_file(Mapping& mapping, const std::filesystem::path& path);
void unmap_file(Mapping& mapping);
void warn(Document& doc, std::string message);
void check_cancel(Document& doc);
void open(Document& doc, const std::filesystem::path& path, std::string_view password = {});
void close(Document& doc);
const Value& resolve(Document& doc, const Value& value);
const Value& entry(Document& doc, const Value& value, std::string_view key);
DecodeResult decode_stream(Document& doc, const Value& stream, bool image = false);
Bytes page_content(Document& doc, const Page& page);
int destination_page(Document& doc, const Value& destination);
Bytes inflate(View data, size_t limit = max_stream);
Bytes ascii_hex(View data);
Bytes ascii85(View data);
Bytes run_length(View data);
Bytes lzw(View data, int early_change = 1);
Bytes predictor(Bytes data, const Value& params);
Bytes digest(std::wstring_view algorithm, View data);
Bytes rc4(View data, View key);
Bytes aes(View data, View key, View iv, bool encrypt, bool padding = false);
std::string prepare_password(std::string_view password);
void authenticate(Document& doc, std::string_view password);
Bytes decrypt(Document& doc, View data, uint32_t object, uint32_t generation, std::string_view method);
void decrypt_strings(Document& doc, Value& value, uint32_t object, uint32_t generation);
std::vector<double> evaluate_function(Document& doc, const Value& function, std::span<const double> input,
                                      int depth = 0);
} // namespace pdf
