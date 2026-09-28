#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

enum class LogFormat {
    Text,
    JsonLines,
};

LogLevel default_log_level();

struct LogField {
    std::string name;
    std::string value;
};

using LogFields = std::vector<LogField>;

struct LogSource {
    const char* file = nullptr;
    i32 line = 0;
};

struct LogEvent {
    u64 time_ns = 0;
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
    const char* file = nullptr;
    i32 line = 0;
    std::string thread;
    LogFields fields;
};

struct LoggerConfig {
    LogLevel min_level = default_log_level();
    LogFormat format = LogFormat::Text;
    bool sdl_sink = true;
    std::ostream* stream = nullptr;
    std::filesystem::path file_path;
    std::vector<LogEvent>* memory_events = nullptr;
};

std::string_view log_level_name(LogLevel level);
std::string_view log_format_name(LogFormat format);
bool parse_log_level(std::string_view text, LogLevel& level);
bool parse_log_format(std::string_view text, LogFormat& format);

LoggerConfig logger_config();
void set_logger_config(LoggerConfig config);
void reset_logger_config_from_environment();

void log(LogLevel level,
         std::string_view category,
         std::string_view message,
         LogFields fields = {},
         LogSource source = {});

std::string format_log_text(const LogEvent& event);
std::string format_log_json_line(const LogEvent& event);

void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

} // namespace kin

#define KIN_LOG_TRACE(category, message) \
    ::kin::log(::kin::LogLevel::Trace, category, message, {}, {__FILE__, __LINE__})
#define KIN_LOG_DEBUG(category, message) \
    ::kin::log(::kin::LogLevel::Debug, category, message, {}, {__FILE__, __LINE__})
#define KIN_LOG_INFO(category, message) \
    ::kin::log(::kin::LogLevel::Info, category, message, {}, {__FILE__, __LINE__})
#define KIN_LOG_WARN(category, message) \
    ::kin::log(::kin::LogLevel::Warn, category, message, {}, {__FILE__, __LINE__})
#define KIN_LOG_ERROR(category, message) \
    ::kin::log(::kin::LogLevel::Error, category, message, {}, {__FILE__, __LINE__})
#define KIN_LOG_FATAL(category, message) \
    ::kin::log(::kin::LogLevel::Fatal, category, message, {}, {__FILE__, __LINE__})

#define KIN_LOG_TRACE_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Trace, category, message, fields, {__FILE__, __LINE__})
#define KIN_LOG_DEBUG_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Debug, category, message, fields, {__FILE__, __LINE__})
#define KIN_LOG_INFO_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Info, category, message, fields, {__FILE__, __LINE__})
#define KIN_LOG_WARN_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Warn, category, message, fields, {__FILE__, __LINE__})
#define KIN_LOG_ERROR_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Error, category, message, fields, {__FILE__, __LINE__})
#define KIN_LOG_FATAL_F(category, message, fields) \
    ::kin::log(::kin::LogLevel::Fatal, category, message, fields, {__FILE__, __LINE__})
