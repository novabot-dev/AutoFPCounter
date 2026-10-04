#include "JsonLite.hpp"

#include <cmath>
#include <cstdlib>

namespace afpc::json {
namespace {

constexpr int kMaxDepth = 32;
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

} // namespace afpc::json