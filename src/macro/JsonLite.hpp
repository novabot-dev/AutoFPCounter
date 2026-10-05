#pragma once

// ---------------------------------------------------------------------------
// JsonLite - a tiny, allocation-bounded JSON reader.
//
// Mega Hack's replay format is the only JSON input the importer accepts. Rather
// than taking a dependency on whatever JSON library the loader happens to
// vendor (and being vulnerable to a parser mismatch at compile time), the
// importer owns a ~200 line reader with a hard recursion depth limit.
//
// Properties that matter here:
//   * Depth limited to 32, so a hostile file cannot smash the stack.
//   * Never throws, never recurses without a bound.
//   * Object lookup is linear but objects in this schema have <= 8 keys.
//   * Returns a status instead of throwing, so callers stay noexcept-friendly.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace afpc::json {

enum class Type : std::uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class Value {
public:
    Value() = default;

    [[nodiscard]] Type type() const noexcept { return m_type; }
    [[nodiscard]] bool isNull() const noexcept { return m_type == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return m_type == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return m_type == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return m_type == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return m_type == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return m_type == Type::Object; }

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept {
        if (m_type == Type::Bool) return m_bool;
        if (m_type == Type::Number) return m_num != 0.0;
        return fallback;
    }

    [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept {
        return m_type == Type::Number ? m_num : fallback;
    }

    [[nodiscard]] const std::string& asString() const noexcept { return m_str; }

    [[nodiscard]] std::size_t size() const noexcept {
        if (m_type == Type::Array) return m_arr.size();
        if (m_type == Type::Object) return m_obj.size();
        return 0;
    }

    [[nodiscard]] const Value& at(std::size_t index) const noexcept {
        if (m_type != Type::Array || index >= m_arr.size()) return s_null();
        return m_arr[index];
    }

    // Object member lookup. Returns nullptr when absent.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        if (m_type != Type::Object) return nullptr;
        for (const auto& kv : m_obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

private:
    friend class Parser;
    // MessagePack is the on-disk form of GDR v1. It decodes into the same tree,
    // so GDR's two dialects share one extraction path.
    friend class MsgPackParser;

    static const Value& s_null() {
        static const Value null;
        return null;
    }

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_num = 0.0;
    std::string m_str;
    std::vector<Value> m_arr;
    std::vector<std::pair<std::string, Value>> m_obj;
};

// Parses `text`. On failure returns a Null value and, if `error` is non-null,
// writes a short human readable reason.
[[nodiscard]] Value parse(std::string_view text, std::string* error);

// Decodes a MessagePack document into the same tree `parse` produces. Needed
// because GDR v1 is serialised with nlohmann's msgpack backend, which is the
// canonical form even though plain JSON is also accepted.
//
// Same guarantees as `parse`: never throws, depth limited to kMaxDepth, and a
// container count is validated against the remaining bytes before allocating.
[[nodiscard]] Value parseMsgPack(std::string_view bytes, std::string* error);

// Shared nesting ceiling for both readers.
inline constexpr int kMaxDepth = 32;

} // namespace afpc::json