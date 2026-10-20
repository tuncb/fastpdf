#include "pdf.h"
#include <bcrypt.h>

namespace pdf {
struct UnicodeRange {
    uint32_t first, last;
};
#include "unicode_ranges.inc"
static bool in_ranges(uint32_t cp, std::span<const UnicodeRange> ranges) {
    auto it = std::lower_bound(ranges.begin(), ranges.end(), cp,
                               [](const UnicodeRange& r, uint32_t value) { return r.last < value; });
    return it != ranges.end() && it->first <= cp;
}
static uint32_t next_codepoint(std::wstring_view text, size_t& position) {
    uint32_t cp = uint16_t(text[position++]);
    if (cp >= 0xd800 && cp <= 0xdbff) {
        require(position < text.size() && text[position] >= 0xdc00 && text[position] <= 0xdfff,
                "Password contains invalid Unicode");
        cp = 0x10000 + ((cp - 0xd800) << 10) + (uint16_t(text[position++]) - 0xdc00);
    } else
        require(cp < 0xdc00 || cp > 0xdfff, "Password contains invalid Unicode");
    return cp;
}
std::string prepare_password(std::string_view password) {
    require(password.size() <= 16384, "Password is too long");
    if (password.empty())
        return {};
    int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password.data(), int(password.size()), nullptr, 0);
    require(length > 0, "Password is not valid UTF-8");
    std::wstring input(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password.data(), int(password.size()), input.data(),
                        length);
    std::wstring mapped;
    mapped.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        size_t begin = i;
        uint32_t cp = next_codepoint(input, i);
        if (in_ranges(cp, sasl_nothing))
            continue;
        if (in_ranges(cp, sasl_space))
            mapped += L' ';
        else
            mapped.append(input, begin, i - begin);
    }
    if (mapped.empty())
        return {};
    length = NormalizeString(NormalizationKC, mapped.data(), int(mapped.size()), nullptr, 0);
    require(length > 0, "Cannot normalize password");
    std::wstring normalized(size_t(length), L'\0');
    int actual =
        NormalizeString(NormalizationKC, mapped.data(), int(mapped.size()), normalized.data(), length);
    require(actual > 0, "Cannot normalize password");
    normalized.resize(size_t(actual));
    bool randal = false, lcat = false;
    uint32_t first = 0, last = 0;
    for (size_t i = 0; i < normalized.size();) {
        uint32_t cp = next_codepoint(normalized, i);
        if (!first)
            first = cp;
        last = cp;
        require(!in_ranges(cp, sasl_prohibited) && !in_ranges(cp, sasl_unassigned),
                "Password contains a prohibited or unassigned Unicode character");
        randal |= in_ranges(cp, sasl_randal);
        lcat |= in_ranges(cp, sasl_lcat);
    }
    require(!randal || (!lcat && in_ranges(first, sasl_randal) && in_ranges(last, sasl_randal)),
            "Password has invalid bidirectional characters");
    return utf8(normalized);
}
static std::string legacy_password(std::string_view password) {
    std::wstring text = utf8(password);
    std::string result;
    for (wchar_t cp : text) {
        bool found = false;
        for (unsigned i = 0; i < 256; ++i) {
            char byte = char(i);
            auto decoded = pdf_text(std::string_view(&byte, 1));
            if (decoded.size() == 1 && decoded[0] == cp) {
                result += byte;
                found = true;
                break;
            }
        }
        if (!found)
            return std::string(password);
    }
    return result;
}
static void crypto_check(NTSTATUS result) {
    require(result >= 0, "Windows cryptographic operation failed");
}
Bytes digest(std::wstring_view algorithm, View data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Bytes result;
    crypto_check(BCryptOpenAlgorithmProvider(&alg, std::wstring(algorithm).c_str(), nullptr, 0));
    try {
        DWORD size = 0, done = 0;
        crypto_check(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size),
                                       &done, 0));
        result.resize(size);
        crypto_check(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
        crypto_check(BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0));
        crypto_check(BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0));
    } catch (...) {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}
Bytes rc4(View data, View key) {
    require(!key.empty() && key.size() <= 256, "Invalid RC4 key");
    std::array<uint8_t, 256> state{};
    for (int i = 0; i < 256; ++i)
        state[i] = uint8_t(i);
    unsigned j = 0;
    for (unsigned i = 0; i < 256; ++i) {
        j = (j + state[i] + key[i % key.size()]) & 255;
        std::swap(state[i], state[j]);
    }
    Bytes out(data.size());
    unsigned i = 0;
    j = 0;
    for (size_t n = 0; n < data.size(); ++n) {
        i = (i + 1) & 255;
        j = (j + state[i]) & 255;
        std::swap(state[i], state[j]);
        out[n] = data[n] ^ state[(unsigned(state[i]) + state[j]) & 255];
    }
    return out;
}
Bytes aes(View data, View key, View iv, bool encrypt, bool padding) {
    require(key.size() == 16 || key.size() == 24 || key.size() == 32, "Invalid AES key length");
    require(iv.empty() || iv.size() == 16, "Invalid AES IV");
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    crypto_check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0));
    Bytes out;
    try {
        auto mode = iv.empty() ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_CBC;
        crypto_check(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                       reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                                       ULONG((wcslen(mode) + 1) * sizeof(wchar_t)), 0));
        crypto_check(BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                                ULONG(key.size()), 0));
        Bytes iv_copy(iv.begin(), iv.end());
        ULONG needed = 0, flags = padding ? BCRYPT_BLOCK_PADDING : 0;
        auto operation = encrypt ? BCryptEncrypt : BCryptDecrypt;
        crypto_check(operation(handle, const_cast<PUCHAR>(data.data()), ULONG(data.size()), nullptr,
                               iv_copy.empty() ? nullptr : iv_copy.data(), ULONG(iv_copy.size()), nullptr, 0,
                               &needed, flags));
        out.resize(needed);
        iv_copy.assign(iv.begin(), iv.end());
        crypto_check(operation(handle, const_cast<PUCHAR>(data.data()), ULONG(data.size()), nullptr,
                               iv_copy.empty() ? nullptr : iv_copy.data(), ULONG(iv_copy.size()), out.data(),
                               ULONG(out.size()), &needed, flags));
        out.resize(needed);
    } catch (...) {
        if (handle)
            BCryptDestroyKey(handle);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw;
    }
    BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
static constexpr uint8_t password_padding[] = {
    0x28, 0xbf, 0x4e, 0x5e, 0x4e, 0x75, 0x8a, 0x41, 0x64, 0x00, 0x4e, 0x56, 0xff, 0xfa, 0x01, 0x08,
    0x2e, 0x2e, 0x00, 0xb6, 0xd0, 0x68, 0x3e, 0x80, 0x2f, 0x0c, 0xa9, 0xfe, 0x64, 0x53, 0x69, 0x7a};
static Bytes padded(View pass) {
    Bytes out(pass.begin(), pass.begin() + std::min<size_t>(32, pass.size()));
    out.insert(out.end(), password_padding, password_padding + 32 - out.size());
    return out;
}
static bool equal_prefix(View a, View b, size_t n) {
    if (a.size() < n || b.size() < n)
        return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < n; ++i)
        difference |= a[i] ^ b[i];
    return difference == 0;
}
static Bytes legacy_key(Document& d, const Value& enc, View pass, int key_length) {
    Bytes data = padded(pass);
    auto& owner = get(enc, "O").text;
    data.insert(data.end(), owner.begin(), owner.end());
    int32_t perms = integer(get(enc, "P"));
    for (int i = 0; i < 4; ++i)
        data.push_back(uint8_t(uint32_t(perms) >> (8 * i)));
    auto& id = at(get(d.trailer, "ID"), 0).text;
    data.insert(data.end(), id.begin(), id.end());
    if (d.security.revision >= 4 && !d.security.metadata)
        data.insert(data.end(), 4, 255);
    Bytes key = digest(BCRYPT_MD5_ALGORITHM, data);
    if (d.security.revision >= 3)
        for (int i = 0; i < 50; ++i)
            key = digest(BCRYPT_MD5_ALGORITHM, View(key).first(key_length));
    key.resize(key_length);
    return key;
}
static bool legacy_valid(Document& d, const Value& enc, View key) {
    if (d.security.revision == 2) {
        auto candidate = rc4(password_padding, key);
        return equal_prefix(candidate, view(get(enc, "U").text), 32);
    }
    Bytes data(std::begin(password_padding), std::end(password_padding));
    auto& id = at(get(d.trailer, "ID"), 0).text;
    data.insert(data.end(), id.begin(), id.end());
    data = rc4(digest(BCRYPT_MD5_ALGORITHM, data), key);
    Bytes mixed(key.begin(), key.end());
    for (int i = 1; i <= 19; ++i) {
        for (size_t j = 0; j < key.size(); ++j)
            mixed[j] = key[j] ^ uint8_t(i);
        data = rc4(data, mixed);
    }
    return equal_prefix(data, view(get(enc, "U").text), 16);
}
static Bytes modern_hash(View pass, View salt, View user, int revision) {
    Bytes data(pass.begin(), pass.end());
    data.insert(data.end(), salt.begin(), salt.end());
    data.insert(data.end(), user.begin(), user.end());
    Bytes key = digest(BCRYPT_SHA256_ALGORITHM, data);
    if (revision == 5)
        return key;
    unsigned round = 0;
    Bytes encrypted;
    do {
        data.assign(pass.begin(), pass.end());
        data.insert(data.end(), key.begin(), key.end());
        data.insert(data.end(), user.begin(), user.end());
        size_t block = data.size();
        data.resize(block * 64);
        for (size_t i = 1; i < 64; ++i)
            std::memcpy(data.data() + block * i, data.data(), block);
        encrypted = aes(data, View(key).first(16), View(key).subspan(16, 16), true);
        unsigned selector = 0;
        for (int i = 0; i < 16; ++i)
            selector += encrypted[i];
        key = digest(selector % 3 == 0   ? BCRYPT_SHA256_ALGORITHM
                     : selector % 3 == 1 ? BCRYPT_SHA384_ALGORITHM
                                         : BCRYPT_SHA512_ALGORITHM,
                     encrypted);
        ++round;
    } while (round < 64 || unsigned(encrypted.back()) > round - 32);
    key.resize(32);
    return key;
}
void authenticate(Document& d, std::string_view password) {
    Value enc = entry(d, d.trailer, "Encrypt");
    require(named(get(enc, "Filter"), "Standard"), "Only the PDF Standard security handler is supported");
    auto& s = d.security;
    s.revision = integer(get(enc, "R"));
    s.version = integer(get(enc, "V"));
    s.metadata = boolean(get(enc, "EncryptMetadata"), true);
    require(s.revision >= 2 && s.revision <= 6, "Unsupported PDF encryption revision");
    if (s.revision <= 4) {
        int length = s.revision == 2 ? 5 : integer(get(enc, "Length"), 40) / 8;
        require(length >= 5 && length <= 16, "Invalid encryption key length");
        bool valid = false;
        auto encoded = legacy_password(password);
        for (const auto& candidate : {encoded, std::string(password)}) {
            auto pass = view(candidate);
            s.key = legacy_key(d, enc, pass, length);
            if (legacy_valid(d, enc, s.key)) {
                valid = true;
                break;
            }
            {
                auto owner_hash = digest(BCRYPT_MD5_ALGORITHM, padded(pass));
                if (s.revision >= 3)
                    for (int i = 0; i < 50; ++i)
                        owner_hash = digest(BCRYPT_MD5_ALGORITHM, owner_hash);
                owner_hash.resize(length);
                Bytes recovered(view(get(enc, "O").text).begin(), view(get(enc, "O").text).end());
                if (s.revision == 2)
                    recovered = rc4(recovered, owner_hash);
                else
                    for (int i = 19; i >= 0; --i) {
                        Bytes k = owner_hash;
                        for (auto& b : k)
                            b ^= uint8_t(i);
                        recovered = rc4(recovered, k);
                    }
                s.key = legacy_key(d, enc, recovered, length);
                if (legacy_valid(d, enc, s.key)) {
                    valid = true;
                    break;
                }
            }
        }
        require(valid, "Password required or incorrect password");
        s.stream_method = s.string_method = "V2";
    } else {
        std::string pass = s.revision == 6 ? prepare_password(password) : std::string(password);
        if (pass.size() > 127)
            pass.resize(127);
        auto u = view(get(enc, "U").text), o = view(get(enc, "O").text);
        require(u.size() >= 48 && o.size() >= 48, "Invalid AES-256 password entries");
        View record = u, extra;
        std::string key_name = "UE";
        auto validation = modern_hash(view(pass), u.subspan(32, 8), {}, s.revision);
        if (!equal_prefix(validation, u, 32)) {
            validation = modern_hash(view(pass), o.subspan(32, 8), u.first(48), s.revision);
            require(equal_prefix(validation, o, 32), "Password required or incorrect password");
            record = o;
            extra = u.first(48);
            key_name = "OE";
        }
        auto key = modern_hash(view(pass), record.subspan(40, 8), extra, s.revision);
        auto wrapped = view(get(enc, key_name).text);
        require(wrapped.size() == 32, "Invalid AES-256 wrapped key");
        std::array<uint8_t, 16> zero{};
        s.key = aes(wrapped, key, zero, false);
        auto perms = view(get(enc, "Perms").text);
        require(perms.size() == 16, "Missing AES-256 permissions block");
        auto plain = aes(perms, s.key, {}, false);
        require(plain[9] == 'a' && plain[10] == 'd' && plain[11] == 'b', "Invalid AES-256 permissions block");
        uint32_t permissions = uint32_t(integer(get(enc, "P")));
        for (int i = 0; i < 4; ++i)
            require(plain[i] == uint8_t(permissions >> (8 * i)), "Inconsistent PDF permissions");
        require(plain[8] == (s.metadata ? 'T' : 'F'), "Inconsistent metadata encryption flag");
        s.stream_method = s.string_method = "AESV3";
    }
    if (s.version >= 4) {
        auto method = [&](std::string_view field) -> std::string {
            auto& name = get(enc, field);
            if (name.kind == Kind::null || named(name, "Identity"))
                return "Identity";
            auto m = get(get(get(enc, "CF"), name.text), "CFM").text;
            require(m == "V2" || m == "AESV2" || m == "AESV3" || m == "None", "Unsupported PDF crypt filter");
            return m == "None" ? "Identity" : m;
        };
        s.stream_method = method("StmF");
        s.string_method = method("StrF");
    }
    s.enabled = true;
    // The encryption dictionary was parsed before authentication; other cached objects
    // may include encrypted strings and must be reloaded with the established key.
    d.objects.clear();
}
Bytes decrypt(Document& d, View data, uint32_t object, uint32_t generation, std::string_view method) {
    if (method == "Identity" || method.empty())
        return Bytes(data.begin(), data.end());
    require(d.security.enabled, "Crypt filter without an authenticated security handler");
    Bytes key = d.security.key;
    if (method != "AESV3") {
        for (int i = 0; i < 3; ++i)
            key.push_back(uint8_t(object >> (8 * i)));
        for (int i = 0; i < 2; ++i)
            key.push_back(uint8_t(generation >> (8 * i)));
        if (method == "AESV2")
            key.insert(key.end(), {'s', 'A', 'l', 'T'});
        auto size = std::min<size_t>(d.security.key.size() + 5, 16);
        key = digest(BCRYPT_MD5_ALGORITHM, key);
        key.resize(size);
    }
    if (method == "V2")
        return rc4(data, key);
    require(method == "AESV2" || method == "AESV3", "Unsupported encryption algorithm");
    require(data.size() >= 32 && data.size() % 16 == 0, "Truncated AES encrypted object");
    return aes(data.subspan(16), key, data.first(16), false, true);
}
void decrypt_strings(Document& d, Value& v, uint32_t object, uint32_t generation) {
    if (v.kind == Kind::string)
        v.text = string(decrypt(d, view(v.text), object, generation, d.security.string_method));
    for (auto& child : v.array)
        decrypt_strings(d, child, object, generation);
    for (auto& e : v.dict)
        decrypt_strings(d, e.second, object, generation);
}
} // namespace pdf
