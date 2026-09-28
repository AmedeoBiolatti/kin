#include <kin/platform/log.hpp>

#include <kin/core/json.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace kin {
namespace {

struct LoggerState {
    LoggerConfig config;
    std::ofstream file;
    bool initialized = false;
};

std::mutex g_logger_mutex;
LoggerState g_logger;

i32 level_rank(LogLevel level) {
    return static_cast<i32>(level);
}

std::string lower_copy(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

u64 now_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string thread_id_string() {
    std::ostringstream out;
    out << std::this_thread::get_id();
    return out.str();
}

SDL_LogPriority to_sdl_priority(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return SDL_LOG_PRIORITY_TRACE;
    case LogLevel::Debug: return SDL_LOG_PRIORITY_DEBUG;
    case LogLevel::Info: return SDL_LOG_PRIORITY_INFO;
    case LogLevel::Warn: return SDL_LOG_PRIORITY_WARN;
    case LogLevel::Error: return SDL_LOG_PRIORITY_ERROR;
    case LogLevel::Fatal: return SDL_LOG_PRIORITY_CRITICAL;
    }
    return SDL_LOG_PRIORITY_INFO;
}

std::string file_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return std::string{path};
    }
    return std::string{path.substr(slash + 1)};
}

LoggerConfig default_config() {
    LoggerConfig config;
    config.min_level = default_log_level();
    config.format = LogFormat::Text;
    config.sdl_sink = true;
    return config;
}

std::optional<LogEvent> apply_environment(LoggerConfig& config) {
    std::string warning;

    if (const char* value = std::getenv("KIN_LOG_LEVEL")) {
        LogLevel parsed{};
        if (parse_log_level(value, parsed)) {
            config.min_level = parsed;
        } else {
            warning = "invalid KIN_LOG_LEVEL, using default";
        }
    }

    if (const char* value = std::getenv("KIN_LOG_FORMAT")) {
        LogFormat parsed{};
        if (parse_log_format(value, parsed)) {
            config.format = parsed;
        } else if (warning.empty()) {
            warning = "invalid KIN_LOG_FORMAT, using default";
        }
    }

    if (const char* value = std::getenv("KIN_LOG_FILE")) {
        if (value[0] != '\0') {
            config.file_path = value;
        }
    }

    if (warning.empty()) {
        return std::nullopt;
    }
    return LogEvent{
        .time_ns = now_ns(),
        .level = LogLevel::Warn,
        .category = "log",
        .message = warning,
        .thread = thread_id_string(),
    };
}

void open_file_sink(LoggerState& state) {
    state.file.close();
    if (!state.config.file_path.empty()) {
        state.file.open(state.config.file_path, std::ios::app);
    }
}

void ensure_initialized_locked() {
    if (g_logger.initialized) {
        return;
    }

    g_logger.config = default_config();
    const std::optional<LogEvent> warning = apply_environment(g_logger.config);
    open_file_sink(g_logger);
    g_logger.initialized = true;

    if (warning) {
        const std::string text = g_logger.config.format == LogFormat::JsonLines
            ? format_log_json_line(*warning)
            : format_log_text(*warning);
        if (g_logger.config.sdl_sink) {
            SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION,
                           to_sdl_priority(warning->level),
                           "%.*s",
                           static_cast<int>(text.size()),
                           text.data());
        }
        if (g_logger.config.stream) {
            *g_logger.config.stream << text << '\n';
        }
        if (g_logger.file) {
            g_logger.file << format_log_json_line(*warning) << '\n';
            g_logger.file.flush();
        }
        if (g_logger.config.memory_events) {
            g_logger.config.memory_events->push_back(*warning);
        }
    }
}

void emit_locked(const LogEvent& event) {
    const std::string text = g_logger.config.format == LogFormat::JsonLines
        ? format_log_json_line(event)
        : format_log_text(event);

    if (g_logger.config.sdl_sink) {
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION,
                       to_sdl_priority(event.level),
                       "%.*s",
                       static_cast<int>(text.size()),
                       text.data());
    }
    if (g_logger.config.stream) {
        *g_logger.config.stream << text << '\n';
    }
    if (g_logger.file) {
        g_logger.file << format_log_json_line(event) << '\n';
        g_logger.file.flush();
    }
    if (g_logger.config.memory_events) {
        g_logger.config.memory_events->push_back(event);
    }
}

} // namespace

LogLevel default_log_level() {
#ifdef NDEBUG
    return LogLevel::Info;
#else
    return LogLevel::Debug;
#endif
}

std::string_view log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "INFO";
}

std::string_view log_format_name(LogFormat format) {
    switch (format) {
    case LogFormat::Text: return "text";
    case LogFormat::JsonLines: return "jsonl";
    }
    return "text";
}

bool parse_log_level(std::string_view text, LogLevel& level) {
    const std::string lower = lower_copy(text);
    if (lower == "trace") {
        level = LogLevel::Trace;
    } else if (lower == "debug") {
        level = LogLevel::Debug;
    } else if (lower == "info") {
        level = LogLevel::Info;
    } else if (lower == "warn" || lower == "warning") {
        level = LogLevel::Warn;
    } else if (lower == "error") {
        level = LogLevel::Error;
    } else if (lower == "fatal" || lower == "critical") {
        level = LogLevel::Fatal;
    } else {
        return false;
    }
    return true;
}

bool parse_log_format(std::string_view text, LogFormat& format) {
    const std::string lower = lower_copy(text);
    if (lower == "text" || lower == "human") {
        format = LogFormat::Text;
    } else if (lower == "jsonl" || lower == "json-lines" || lower == "jsonlines") {
        format = LogFormat::JsonLines;
    } else {
        return false;
    }
    return true;
}

LoggerConfig logger_config() {
    std::scoped_lock lock{g_logger_mutex};
    ensure_initialized_locked();
    return g_logger.config;
}

void set_logger_config(LoggerConfig config) {
    std::scoped_lock lock{g_logger_mutex};
    g_logger.config = std::move(config);
    g_logger.initialized = true;
    open_file_sink(g_logger);
}

void reset_logger_config_from_environment() {
    std::scoped_lock lock{g_logger_mutex};
    g_logger.config = default_config();
    const std::optional<LogEvent> warning = apply_environment(g_logger.config);
    g_logger.initialized = true;
    open_file_sink(g_logger);
    if (warning) {
        emit_locked(*warning);
    }
}

void log(LogLevel level,
         std::string_view category,
         std::string_view message,
         LogFields fields,
         LogSource source) {
    std::scoped_lock lock{g_logger_mutex};
    ensure_initialized_locked();
    if (level_rank(level) < level_rank(g_logger.config.min_level)) {
        return;
    }

    LogEvent event{
        .time_ns = now_ns(),
        .level = level,
        .category = std::string{category},
        .message = std::string{message},
        .file = source.file,
        .line = source.line,
        .thread = thread_id_string(),
        .fields = std::move(fields),
    };
    emit_locked(event);
}

std::string format_log_text(const LogEvent& event) {
    std::ostringstream out;
    const auto millis = event.time_ns / 1'000'000;
    out << '[' << millis << "ms] "
        << log_level_name(event.level) << ' '
        << event.category << ": "
        << event.message;
    for (const LogField& field : event.fields) {
        out << ' ' << field.name << '=' << field.value;
    }
    if (event.file && event.line > 0) {
        out << " (" << file_name(event.file) << ':' << event.line << ')';
    }
    return out.str();
}

std::string format_log_json_line(const LogEvent& event) {
    std::ostringstream out;
    out << "{\"schema\":\"kin.log/1\"";
    out << ",\"time_ns\":" << event.time_ns;
    out << ",\"level\":";
    write_json_escaped(out, log_level_name(event.level));
    out << ",\"category\":";
    write_json_escaped(out, event.category);
    out << ",\"message\":";
    write_json_escaped(out, event.message);
    out << ",\"file\":";
    if (event.file) {
        write_json_escaped(out, event.file);
    } else {
        out << "null";
    }
    out << ",\"line\":" << event.line;
    out << ",\"thread\":";
    write_json_escaped(out, event.thread);
    out << ",\"fields\":{";
    for (std::size_t i = 0; i < event.fields.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        write_json_escaped(out, event.fields[i].name);
        out << ':';
        write_json_escaped(out, event.fields[i].value);
    }
    out << "}}";
    return out.str();
}

void log_debug(std::string_view message) {
    log(LogLevel::Debug, "app", message);
}

void log_info(std::string_view message) {
    log(LogLevel::Info, "app", message);
}

void log_warn(std::string_view message) {
    log(LogLevel::Warn, "app", message);
}

void log_error(std::string_view message) {
    log(LogLevel::Error, "app", message);
}

} // namespace kin
