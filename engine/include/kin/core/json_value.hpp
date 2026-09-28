#pragma once

#include <kin/core/types.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class JsonWriter;

// Minimal immutable JSON document model plus a hand-rolled parser, sized for
// reading request payloads (server commands) rather than arbitrary large data.
// The companion JsonWriter handles serialization.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue() = default;
    JsonValue(bool value) : _type(Type::Bool), _bool(value) {}
    JsonValue(f64 value) : _type(Type::Number), _number(value) {}
    JsonValue(std::string value) : _type(Type::String), _string(std::move(value)) {}
    JsonValue(Array value) : _type(Type::Array), _array(std::move(value)) {}
    JsonValue(Object value) : _type(Type::Object), _object(std::move(value)) {}

    Type type() const { return _type; }
    bool is_null() const { return _type == Type::Null; }
    bool is_bool() const { return _type == Type::Bool; }
    bool is_number() const { return _type == Type::Number; }
    bool is_string() const { return _type == Type::String; }
    bool is_array() const { return _type == Type::Array; }
    bool is_object() const { return _type == Type::Object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? _bool : fallback; }
    f64 as_number(f64 fallback = 0.0) const { return is_number() ? _number : fallback; }
    i64 as_int(i64 fallback = 0) const {
        return is_number() ? static_cast<i64>(_number) : fallback;
    }
    const std::string& as_string(const std::string& fallback = empty_string()) const {
        return is_string() ? _string : fallback;
    }
    // Reject a temporary fallback: the function returns a reference, so binding
    // the result would dangle once the temporary dies. Callers needing a literal
    // default should hold it in a named std::string first.
    const std::string& as_string(std::string&& fallback) const = delete;

    const Array& items() const { return _array; }
    const Object& members() const { return _object; }

    // Object member lookup; returns nullptr when absent or not an object.
    const JsonValue* find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }

    // Convenience accessors with defaults for object members.
    i64 int_at(std::string_view key, i64 fallback = 0) const;
    f64 number_at(std::string_view key, f64 fallback = 0.0) const;
    bool bool_at(std::string_view key, bool fallback = false) const;
    std::string string_at(std::string_view key, std::string_view fallback = {}) const;

private:
    static const std::string& empty_string();

    Type _type = Type::Null;
    bool _bool = false;
    f64 _number = 0.0;
    std::string _string;
    Array _array;
    Object _object;
};

struct JsonParseResult {
    std::optional<JsonValue> value;
    std::string error;     // empty when value is present
    std::size_t position = 0;

    bool ok() const { return value.has_value(); }
};

JsonParseResult parse_json(std::string_view text);

// Serializes a parsed value back out through a JsonWriter.
void write_json(JsonWriter& out, const JsonValue& value);

} // namespace kin
