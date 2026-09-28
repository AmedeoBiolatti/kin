#include <kin/core/json_value.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <kin/core/json.hpp>

namespace kin {

void write_json(JsonWriter& out, const JsonValue& value) {
    switch (value.type()) {
    case JsonValue::Type::Null:
        out.value_null();
        break;
    case JsonValue::Type::Bool:
        out.value(value.as_bool());
        break;
    case JsonValue::Type::Number:
        out.value(value.as_number());
        break;
    case JsonValue::Type::String:
        out.value(std::string_view{value.as_string()});
        break;
    case JsonValue::Type::Array:
        out.begin_array();
        for (const JsonValue& item : value.items()) {
            write_json(out, item);
        }
        out.end_array();
        break;
    case JsonValue::Type::Object:
        out.begin_object();
        for (const auto& [key, member] : value.members()) {
            out.key(std::string_view{key});
            write_json(out, member);
        }
        out.end_object();
        break;
    }
}

const std::string& JsonValue::empty_string() {
    static const std::string value;
    return value;
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    const auto found = _object.find(std::string{key});
    return found == _object.end() ? nullptr : &found->second;
}

i64 JsonValue::int_at(std::string_view key, i64 fallback) const {
    const JsonValue* value = find(key);
    return value ? value->as_int(fallback) : fallback;
}

f64 JsonValue::number_at(std::string_view key, f64 fallback) const {
    const JsonValue* value = find(key);
    return value ? value->as_number(fallback) : fallback;
}

bool JsonValue::bool_at(std::string_view key, bool fallback) const {
    const JsonValue* value = find(key);
    return value ? value->as_bool(fallback) : fallback;
}

std::string JsonValue::string_at(std::string_view key, std::string_view fallback) const {
    const JsonValue* value = find(key);
    if (value && value->is_string()) {
        return value->as_string();
    }
    return std::string{fallback};
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : _text(text) {}

    JsonParseResult run() {
        skip_ws();
        JsonValue value;
        if (!parse_value(value)) {
            return {std::nullopt, _error, _pos};
        }
        skip_ws();
        if (_pos != _text.size()) {
            return {std::nullopt, "trailing characters after JSON value", _pos};
        }
        return {std::move(value), {}, _pos};
    }

private:
    bool fail(std::string message) {
        if (_error.empty()) {
            _error = std::move(message);
        }
        return false;
    }

    bool eof() const { return _pos >= _text.size(); }
    char peek() const { return _text[_pos]; }

    void skip_ws() {
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++_pos;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out) {
        skip_ws();
        if (eof()) {
            return fail("unexpected end of input");
        }
        switch (peek()) {
        case '{': return parse_object(out);
        case '[': return parse_array(out);
        case '"': {
            std::string text;
            if (!parse_string(text)) {
                return false;
            }
            out = JsonValue{std::move(text)};
            return true;
        }
        case 't': case 'f': return parse_bool(out);
        case 'n': return parse_null(out);
        default: return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out) {
        ++_pos; // consume '{'
        JsonValue::Object object;
        skip_ws();
        if (!eof() && peek() == '}') {
            ++_pos;
            out = JsonValue{std::move(object)};
            return true;
        }
        while (true) {
            skip_ws();
            if (eof() || peek() != '"') {
                return fail("expected string key in object");
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (eof() || peek() != ':') {
                return fail("expected ':' after object key");
            }
            ++_pos;
            JsonValue value;
            if (!parse_value(value)) {
                return false;
            }
            object.insert_or_assign(std::move(key), std::move(value));
            skip_ws();
            if (eof()) {
                return fail("unterminated object");
            }
            if (peek() == ',') {
                ++_pos;
                continue;
            }
            if (peek() == '}') {
                ++_pos;
                break;
            }
            return fail("expected ',' or '}' in object");
        }
        out = JsonValue{std::move(object)};
        return true;
    }

    bool parse_array(JsonValue& out) {
        ++_pos; // consume '['
        JsonValue::Array array;
        skip_ws();
        if (!eof() && peek() == ']') {
            ++_pos;
            out = JsonValue{std::move(array)};
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parse_value(value)) {
                return false;
            }
            array.push_back(std::move(value));
            skip_ws();
            if (eof()) {
                return fail("unterminated array");
            }
            if (peek() == ',') {
                ++_pos;
                continue;
            }
            if (peek() == ']') {
                ++_pos;
                break;
            }
            return fail("expected ',' or ']' in array");
        }
        out = JsonValue{std::move(array)};
        return true;
    }

    bool parse_string(std::string& out) {
        ++_pos; // consume opening quote
        std::string result;
        while (!eof()) {
            const char c = _text[_pos++];
            if (c == '"') {
                out = std::move(result);
                return true;
            }
            if (c == '\\') {
                if (eof()) {
                    return fail("unterminated escape");
                }
                const char esc = _text[_pos++];
                switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (!parse_unicode_escape(result)) {
                        return false;
                    }
                    break;
                }
                default: return fail("invalid escape sequence");
                }
            } else {
                result.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parse_unicode_escape(std::string& out) {
        if (_pos + 4 > _text.size()) {
            return fail("truncated \\u escape");
        }
        u32 code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = _text[_pos++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<u32>(c - 'A' + 10);
            } else {
                return fail("invalid hex in \\u escape");
            }
        }
        // Encode the basic-plane code point as UTF-8. Surrogate pairs are not
        // expected in command payloads and are passed through as-is.
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    bool parse_bool(JsonValue& out) {
        if (_text.compare(_pos, 4, "true") == 0) {
            _pos += 4;
            out = JsonValue{true};
            return true;
        }
        if (_text.compare(_pos, 5, "false") == 0) {
            _pos += 5;
            out = JsonValue{false};
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_null(JsonValue& out) {
        if (_text.compare(_pos, 4, "null") == 0) {
            _pos += 4;
            out = JsonValue{};
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = _pos;
        if (!eof() && (peek() == '-' || peek() == '+')) {
            ++_pos;
        }
        while (!eof()) {
            const char c = peek();
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++_pos;
            } else {
                break;
            }
        }
        if (_pos == start) {
            return fail("invalid value");
        }
        f64 number = 0.0;
        const char* begin = _text.data() + start;
        const char* end = _text.data() + _pos;
        const auto parsed = std::from_chars(begin, end, number);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return fail("invalid number");
        }
        out = JsonValue{number};
        return true;
    }

    std::string_view _text;
    std::size_t _pos = 0;
    std::string _error;
};

} // namespace

JsonParseResult parse_json(std::string_view text) {
    return Parser{text}.run();
}

} // namespace kin
