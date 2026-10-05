#include "JsonLite.hpp"

#include <cmath>
#include <cstdlib>

namespace afpc::json {
namespace {

// kMaxDepth lives in the header so the MessagePack reader shares the ceiling.
constexpr std::size_t kMaxElements = 4'000'000;

inline bool isWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

} // namespace

class Parser {
public:
    Parser(std::string_view text, std::string* error) : m_text(text), m_error(error) {}

    bool run(Value& out) {
        skipWhitespace();
        if (!parseValue(out, 0)) return false;
        skipWhitespace();
        if (m_pos != m_text.size()) {
            return fail("trailing content after root value");
        }
        return true;
    }

private:
    bool fail(const char* what) {
        if (m_error && m_error->empty()) {
            *m_error = std::string(what) + " at offset " + std::to_string(m_pos);
        }
        return false;
    }

    void skipWhitespace() noexcept {
        while (m_pos < m_text.size() && isWhitespace(m_text[m_pos])) ++m_pos;
    }

    bool eof() const noexcept { return m_pos >= m_text.size(); }

    char peek() const noexcept { return m_text[m_pos]; }

    bool expect(char c) {
        if (eof() || m_text[m_pos] != c) return fail("unexpected character");
        ++m_pos;
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
        if (eof()) return fail("unexpected end of input");

        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out.m_type = Type::String;
                out.m_str = std::move(s);
                return true;
            }
            case 't':
                if (m_text.compare(m_pos, 4, "true") == 0) {
                    m_pos += 4;
                    out.m_type = Type::Bool;
                    out.m_bool = true;
                    return true;
                }
                return fail("invalid literal");
            case 'f':
                if (m_text.compare(m_pos, 5, "false") == 0) {
                    m_pos += 5;
                    out.m_type = Type::Bool;
                    out.m_bool = false;
                    return true;
                }
                return fail("invalid literal");
            case 'n':
                if (m_text.compare(m_pos, 4, "null") == 0) {
                    m_pos += 4;
                    out.m_type = Type::Null;
                    return true;
                }
                return fail("invalid literal");
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        if (!expect('{')) return false;
        out.m_type = Type::Object;

        skipWhitespace();
        if (eof()) return fail("unterminated object");
        if (peek() == '}') {
            ++m_pos;
            return true;
        }

        for (;;) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!expect(':')) return false;
            skipWhitespace();

            Value child;
            if (!parseValue(child, depth + 1)) return false;

            // Last one wins on duplicate keys - matches common JSON readers.
            bool replaced = false;
            for (auto& kv : out.m_obj) {
                if (kv.first == key) {
                    kv.second = std::move(child);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                if (out.m_obj.size() >= kMaxElements) return fail("object too large");
                out.m_obj.emplace_back(std::move(key), std::move(child));
            }

            skipWhitespace();
            if (eof()) return fail("unterminated object");
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == '}') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value& out, int depth) {
        if (!expect('[')) return false;
        out.m_type = Type::Array;

        skipWhitespace();
        if (eof()) return fail("unterminated array");
        if (peek() == ']') {
            ++m_pos;
            return true;
        }

        for (;;) {
            skipWhitespace();
            Value child;
            if (!parseValue(child, depth + 1)) return false;
            if (out.m_arr.size() >= kMaxElements) return fail("array too large");
            out.m_arr.push_back(std::move(child));

            skipWhitespace();
            if (eof()) return fail("unterminated array");
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == ']') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseString(std::string& out) {
        if (!expect('"')) return false;
        out.clear();

        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = m_text[m_pos++];

            if (c == '"') return true;

            if (c != '\\') {
                // Raw control characters are technically illegal in JSON but
                // appear in the wild; accept them rather than failing.
                out.push_back(c);
                continue;
            }

            if (eof()) return fail("unterminated escape");
            const char e = m_text[m_pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_text.size()) return fail("truncated \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = m_text[m_pos + static_cast<std::size_t>(i)];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("invalid \\u escape");
                    }
                    m_pos += 4;

                    // Minimal UTF-8 encode. Surrogate pairs are passed through
                    // as-is; this schema never needs them.
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    return fail("invalid escape character");
            }
        }
    }

    bool parseNumber(Value& out) {
        const std::size_t start = m_pos;

        if (!eof() && (peek() == '-' || peek() == '+')) ++m_pos;

        bool anyDigits = false;
        while (!eof() && isDigit(peek())) {
            ++m_pos;
            anyDigits = true;
        }
        if (!eof() && peek() == '.') {
            ++m_pos;
            while (!eof() && isDigit(peek())) {
                ++m_pos;
                anyDigits = true;
            }
        }
        if (!anyDigits) return fail("invalid number");

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++m_pos;
            if (!eof() && (peek() == '-' || peek() == '+')) ++m_pos;
            bool expDigits = false;
            while (!eof() && isDigit(peek())) {
                ++m_pos;
                expDigits = true;
            }
            if (!expDigits) return fail("invalid exponent");
        }

        // strtod on a NUL-terminated copy of just the token. The token is
        // short and bounded by the input length.
        const std::string token(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == token.c_str()) return fail("invalid number");

        // The JSON grammar excludes inf/nan literals, but a huge exponent can
        // still overflow strtod to inf. Normalise that to 0 rather than letting
        // a non-finite value reach the maths engine.
        out.m_type = Type::Number;
        out.m_num = std::isfinite(value) ? value : 0.0;
        return true;
    }

    std::string_view m_text;
    std::string* m_error = nullptr;
    std::size_t m_pos = 0;
};

Value parse(std::string_view text, std::string* error) {
    if (error) error->clear();

    Value out;
    Parser parser(text, error);
    if (!parser.run(out)) {
        if (error && error->empty()) *error = "invalid JSON";
        return Value{};
    }
    return out;
}

// ---------------------------------------------------------------------------
// MessagePack
//
// GDR v1 is written with nlohmann's msgpack backend, so this is the path real
// .gdr files actually take. The encoding is byte-oriented and self-delimiting,
// which makes a bounds-checked reader straightforward:
//
//   positive fixint  0x00..0x7f   the value itself
//   fixmap           0x80..0x8f   low nibble = pair count
//   fixarray         0x90..0x9f   low nibble = element count
//   fixstr           0xa0..0xbf   low 5 bits = length
//   nil / false / true          0xc0 / 0xc2 / 0xc3
//   bin8/16/32                  0xc4..0xc6
//   ext8/16/32                  0xc7..0xc9
//   float32 / float64           0xca / 0xcb
//   uint8/16/32/64              0xcc..0xcf
//   int8/16/32/64               0xd0..0xd3
//   fixext1/2/4/8/16            0xd4..0xd8
//   str8/16/32                  0xd9..0xdb
//   array16/32                  0xdc..0xdd
//   map16/32                    0xde..0xdf
//   negative fixint  0xe0..0xff   value minus 256
//
// Integers widen to double because the tree has one numeric slot; GDR only ever
// stores small counts and frames, so nothing is lost. Binary and extension
// payloads are skipped rather than materialised - no GDR field uses them.
// ---------------------------------------------------------------------------

class MsgPackParser {
public:
    MsgPackParser(std::string_view bytes, std::string* error)
        : m_bytes(bytes), m_error(error) {}

    bool run(Value& out) {
        if (!parseValue(out, 0)) return false;
        if (m_pos != m_bytes.size()) return fail("trailing bytes after root value");
        return true;
    }

private:
    bool fail(const char* what) {
        if (m_error && m_error->empty()) {
            *m_error = std::string(what) + " at offset " + std::to_string(m_pos);
        }
        return false;
    }

    bool eof() const noexcept { return m_pos >= m_bytes.size(); }

    std::uint8_t byte() noexcept {
        return static_cast<std::uint8_t>(m_bytes[m_pos++]);
    }

    // Big endian, which is what MessagePack uses for every multi-byte scalar.
    std::uint64_t be(int width) noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < width; ++i) {
            if (eof()) {
                m_pos = m_bytes.size();
                return 0;
            }
            v = (v << 8) | byte();
        }
        return v;
    }

    // A declared length is only honoured if the bytes for it could possibly be
    // present. Each element costs at least one byte, so a count larger than the
    // remaining buffer is corruption rather than a real document.
    bool reserveOk(std::uint64_t count, int minBytesPerElement) noexcept {
        const std::uint64_t remaining = m_bytes.size() - m_pos;
        const std::uint64_t need = count * static_cast<std::uint64_t>(minBytesPerElement);
        if (need > remaining) return fail("declared length exceeds remaining buffer");
        if (count > kMaxElements) return fail("element count exceeds the safety ceiling");
        return true;
    }

    bool takeString(std::uint64_t len, std::string& out) {
        if (!reserveOk(len, 1)) return false;
        out.assign(m_bytes.data() + m_pos, static_cast<std::size_t>(len));
        m_pos += static_cast<std::size_t>(len);
        return true;
    }

    bool skipPayload(std::uint64_t len) {
        if (len > m_bytes.size() - m_pos) return fail("truncated payload");
        m_pos += static_cast<std::size_t>(len);
        return true;
    }

    bool parseMap(Value& out, std::uint64_t pairs, int depth) {
        if (!reserveOk(pairs, 2)) return false;
        out.m_type = Type::Object;
        out.m_obj.clear();
        out.m_obj.reserve(static_cast<std::size_t>(pairs < 1024 ? pairs : 1024));

        for (std::uint64_t i = 0; i < pairs; ++i) {
            std::string key;
            Value v;
            if (!parseStringValue(key)) return false;
            if (!parseValue(v, depth + 1)) return false;
            out.m_obj.emplace_back(std::move(key), std::move(v));
        }
        return true;
    }

    bool parseArray(Value& out, std::uint64_t count, int depth) {
        if (!reserveOk(count, 1)) return false;
        out.m_type = Type::Array;
        out.m_arr.clear();
        out.m_arr.reserve(static_cast<std::size_t>(count < 1024 ? count : 1024));

        for (std::uint64_t i = 0; i < count; ++i) {
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            out.m_arr.push_back(std::move(v));
        }
        return true;
    }

    // Map keys in a msgpack document are strings in every real encoder, but the
    // spec permits any type. Anything else is skipped so a non-string key
    // cannot desynchronise the reader.
    bool parseStringValue(std::string& out) {
        if (eof()) return fail("unexpected end of input");
        const std::uint8_t tag = static_cast<std::uint8_t>(m_bytes[m_pos]);

        if ((tag & 0xe0) == 0xa0) { // fixstr
            m_pos++;
            return takeString(tag & 0x1f, out);
        }
        if (tag == 0xd9 || tag == 0xda || tag == 0xdb) { // str8 / 16 / 32
            m_pos++;
            const int width = tag == 0xd9 ? 1 : (tag == 0xda ? 2 : 4);
            const std::uint64_t len = be(width);
            if (eof() && len != 0) return fail("truncated string length");
            return takeString(len, out);
        }

        Value discard;
        return parseValue(discard, 1);
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
        if (eof()) return fail("unexpected end of input");

        const std::uint8_t tag = byte();

        // --- single-byte forms -------------------------------------------
        if (tag <= 0x7f) { // positive fixint
            out.m_type = Type::Number;
            out.m_num = static_cast<double>(tag);
            return true;
        }
        if (tag >= 0xe0) { // negative fixint
            out.m_type = Type::Number;
            out.m_num = static_cast<double>(static_cast<int>(tag) - 256);
            return true;
        }
        if ((tag & 0xf0) == 0x80) { // fixmap
            return parseMap(out, tag & 0x0f, depth);
        }
        if ((tag & 0xf0) == 0x90) { // fixarray
            return parseArray(out, tag & 0x0f, depth);
        }
        if ((tag & 0xe0) == 0xa0) { // fixstr
            std::string s;
            if (!takeString(tag & 0x1f, s)) return false;
            out.m_type = Type::String;
            out.m_str = std::move(s);
            return true;
        }

        switch (tag) {
            case 0xc0: // nil
                out.m_type = Type::Null;
                return true;
            case 0xc2: // false
                out.m_type = Type::Bool;
                out.m_bool = false;
                return true;
            case 0xc3: // true
                out.m_type = Type::Bool;
                out.m_bool = true;
                return true;

            // --- integers -------------------------------------------------
            case 0xcc: out.m_type = Type::Number; out.m_num = static_cast<double>(be(1)); return true;
            case 0xcd: out.m_type = Type::Number; out.m_num = static_cast<double>(be(2)); return true;
            case 0xce: out.m_type = Type::Number; out.m_num = static_cast<double>(be(4)); return true;
            case 0xcf: out.m_type = Type::Number; out.m_num = static_cast<double>(be(8)); return true;
            case 0xd0: out.m_type = Type::Number; out.m_num = static_cast<double>(static_cast<std::int8_t>(be(1))); return true;
            case 0xd1: out.m_type = Type::Number; out.m_num = static_cast<double>(static_cast<std::int16_t>(be(2))); return true;
            case 0xd2: out.m_type = Type::Number; out.m_num = static_cast<double>(static_cast<std::int32_t>(be(4))); return true;
            case 0xd3: out.m_type = Type::Number; out.m_num = static_cast<double>(static_cast<std::int64_t>(be(8))); return true;

            // --- floats ---------------------------------------------------
            case 0xca: {
                const std::uint32_t bits = static_cast<std::uint32_t>(be(4));
                float f = 0.0f;
                static_assert(sizeof(f) == sizeof(bits));
                __builtin_memcpy(&f, &bits, sizeof(f));
                out.m_type = Type::Number;
                // Reject NaN/inf here rather than letting them reach the maths.
                out.m_num = std::isfinite(f) ? static_cast<double>(f) : 0.0;
                return true;
            }
            case 0xcb: {
                const std::uint64_t bits = be(8);
                double d = 0.0;
                static_assert(sizeof(d) == sizeof(bits));
                __builtin_memcpy(&d, &bits, sizeof(d));
                out.m_type = Type::Number;
                out.m_num = std::isfinite(d) ? d : 0.0;
                return true;
            }

            // --- str ------------------------------------------------------
            case 0xd9: case 0xda: case 0xdb: {
                const int width = tag == 0xd9 ? 1 : (tag == 0xda ? 2 : 4);
                const std::uint64_t len = be(width);
                std::string s;
                if (!takeString(len, s)) return false;
                out.m_type = Type::String;
                out.m_str = std::move(s);
                return true;
            }

            // --- array / map with explicit width ----------------------------
            case 0xdc: return parseArray(out, be(2), depth);
            case 0xdd: return parseArray(out, be(4), depth);
            case 0xde: return parseMap(out, be(2), depth);
            case 0xdf: return parseMap(out, be(4), depth);

            // --- bin: skipped, nothing in the GDR schema uses it ----------
            case 0xc4: return skipPayload(be(1));
            case 0xc5: return skipPayload(be(2));
            case 0xc6: return skipPayload(be(4));

            // --- ext: skipped for the same reason --------------------------
            case 0xc7: { const std::uint8_t n = static_cast<std::uint8_t>(be(1)); return skipPayload(static_cast<std::uint64_t>(n) + 1); }
            case 0xc8: { const std::uint16_t n = static_cast<std::uint16_t>(be(2)); return skipPayload(static_cast<std::uint64_t>(n) + 1); }
            case 0xc9: { const std::uint32_t n = static_cast<std::uint32_t>(be(4)); return skipPayload(static_cast<std::uint64_t>(n) + 1); }
            case 0xd4: return skipPayload(2); // fixext1  : 1 type byte + 1 payload
            case 0xd5: return skipPayload(3); // fixext2
            case 0xd6: return skipPayload(5); // fixext4
            case 0xd7: return skipPayload(9); // fixext8
            case 0xd8: return skipPayload(17); // fixext16

            default:
                return fail("unknown MessagePack type tag");
        }
    }

    std::string_view m_bytes;
    std::string* m_error = nullptr;
    std::size_t m_pos = 0;
};

Value parseMsgPack(std::string_view bytes, std::string* error) {
    if (error) error->clear();

    Value out;
    MsgPackParser parser(bytes, error);
    if (!parser.run(out)) {
        if (error && error->empty()) *error = "invalid MessagePack";
        return Value{};
    }
    return out;
}

} // namespace afpc::json