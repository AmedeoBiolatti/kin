#pragma once

#include <kin/core/types.hpp>

#include <iosfwd>
#include <string_view>
#include <vector>

namespace kin {

// Minimal streaming JSON writer. Tracks object/array nesting so callers do not
// manage commas, colons, or indentation by hand. Output is pretty-printed by
// default (two-space indent) which keeps run reports readable for both agents
// and humans.
//
// Usage:
//   JsonWriter json(out);
//   json.begin_object();
//   json.field("status", "ok");
//   json.key("scenes").begin_array();
//   json.begin_object().field("name", "Play").end_object();
//   json.end_array();
//   json.end_object();
class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out, bool pretty = true);

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();

    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text);
    JsonWriter& value(bool boolean);
    JsonWriter& value(i32 number);
    JsonWriter& value(i64 number);
    JsonWriter& value(u32 number);
    JsonWriter& value(u64 number);
    JsonWriter& value(f64 number);
    JsonWriter& value_null();

    // Splices already-serialized JSON text in as a value (e.g. output from
    // flecs to_json). The caller is responsible for it being valid JSON.
    JsonWriter& raw(std::string_view json);

    JsonWriter& field(std::string_view name, std::string_view text);
    JsonWriter& field(std::string_view name, const char* text);
    JsonWriter& field(std::string_view name, bool boolean);
    JsonWriter& field(std::string_view name, i32 number);
    JsonWriter& field(std::string_view name, i64 number);
    JsonWriter& field(std::string_view name, u32 number);
    JsonWriter& field(std::string_view name, u64 number);
    JsonWriter& field(std::string_view name, f64 number);

private:
    enum class Scope { Object, Array };

    struct Frame {
        Scope scope;
        bool nonempty;
    };

    void before_value();
    void write_newline_indent();
    void write_string(std::string_view text);

    std::ostream& _out;
    bool _pretty;
    std::vector<Frame> _stack;
    bool _expect_value = false;
};

// Writes `text` to `out` as a JSON string literal: surrounding quotes plus
// escaping of quotes, backslashes, and control characters (as \u00XX). Shared by
// JsonWriter and other serializers (e.g. structured logging) that emit JSON
// strings without going through a JsonWriter.
void write_json_escaped(std::ostream& out, std::string_view text);

} // namespace kin
