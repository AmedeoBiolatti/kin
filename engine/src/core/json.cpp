#include <kin/core/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <ostream>

namespace kin {
namespace {

void write_number(std::ostream& out, auto number) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    out.write(buffer.data(), result.ptr - buffer.data());
}

} // namespace

JsonWriter::JsonWriter(std::ostream& out, bool pretty)
    : _out(out), _pretty(pretty) {
}

void JsonWriter::write_newline_indent() {
    if (!_pretty) {
        return;
    }
    _out << '\n';
    for (std::size_t i = 0; i < _stack.size(); ++i) {
        _out << "  ";
    }
}

void JsonWriter::before_value() {
    if (_expect_value) {
        // A key() call already wrote the separator; the value follows directly.
        _expect_value = false;
        return;
    }
    if (_stack.empty()) {
        return;
    }
    Frame& frame = _stack.back();
    if (frame.nonempty) {
        _out << ',';
    }
    frame.nonempty = true;
    write_newline_indent();
}

void write_json_escaped(std::ostream& out, std::string_view text) {
    out << '"';
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr std::array<char, 16> hex = {
                    '0', '1', '2', '3', '4', '5', '6', '7',
                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                };
                out << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
            } else {
                out << raw;
            }
            break;
        }
    }
    out << '"';
}

void JsonWriter::write_string(std::string_view text) {
    write_json_escaped(_out, text);
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    _out << '{';
    _stack.push_back({Scope::Object, false});
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    const Frame frame = _stack.back();
    _stack.pop_back();
    if (frame.nonempty) {
        write_newline_indent();
    }
    _out << '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    _out << '[';
    _stack.push_back({Scope::Array, false});
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    const Frame frame = _stack.back();
    _stack.pop_back();
    if (frame.nonempty) {
        write_newline_indent();
    }
    _out << ']';
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    Frame& frame = _stack.back();
    if (frame.nonempty) {
        _out << ',';
    }
    frame.nonempty = true;
    write_newline_indent();
    write_string(name);
    _out << ':';
    if (_pretty) {
        _out << ' ';
    }
    _expect_value = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    before_value();
    write_string(text);
    return *this;
}

JsonWriter& JsonWriter::value(const char* text) {
    return value(std::string_view{text});
}

JsonWriter& JsonWriter::value(bool boolean) {
    before_value();
    _out << (boolean ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::value(i32 number) {
    before_value();
    write_number(_out, number);
    return *this;
}

JsonWriter& JsonWriter::value(i64 number) {
    before_value();
    write_number(_out, number);
    return *this;
}

JsonWriter& JsonWriter::value(u32 number) {
    before_value();
    write_number(_out, number);
    return *this;
}

JsonWriter& JsonWriter::value(u64 number) {
    before_value();
    write_number(_out, number);
    return *this;
}

JsonWriter& JsonWriter::value(f64 number) {
    before_value();
    // std::to_chars renders NaN/Inf as "nan"/"inf", which are not valid JSON
    // tokens and would corrupt the whole document. Emit null for non-finite
    // values so a stray NaN in game state can't break an entire snapshot.
    if (std::isfinite(number)) {
        write_number(_out, number);
    } else {
        _out << "null";
    }
    return *this;
}

JsonWriter& JsonWriter::value_null() {
    before_value();
    _out << "null";
    return *this;
}

JsonWriter& JsonWriter::raw(std::string_view json) {
    before_value();
    if (json.empty()) {
        _out << "null";
    } else {
        _out << json;
    }
    return *this;
}

JsonWriter& JsonWriter::field(std::string_view name, std::string_view text) {
    return key(name).value(text);
}

JsonWriter& JsonWriter::field(std::string_view name, const char* text) {
    return key(name).value(text);
}

JsonWriter& JsonWriter::field(std::string_view name, bool boolean) {
    return key(name).value(boolean);
}

JsonWriter& JsonWriter::field(std::string_view name, i32 number) {
    return key(name).value(number);
}

JsonWriter& JsonWriter::field(std::string_view name, i64 number) {
    return key(name).value(number);
}

JsonWriter& JsonWriter::field(std::string_view name, u32 number) {
    return key(name).value(number);
}

JsonWriter& JsonWriter::field(std::string_view name, u64 number) {
    return key(name).value(number);
}

JsonWriter& JsonWriter::field(std::string_view name, f64 number) {
    return key(name).value(number);
}

} // namespace kin
