#pragma once

#include "Types.hpp"
#include <cstdio>
#include <cstdarg>
#include <string>

namespace ps96 {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5
};

class Log {
public:
    static void set_level(LogLevel level);
    static LogLevel level();
    static void set_cpu_trace(bool enabled);
    static bool cpu_trace();
    static bool open_file(const std::string& path);
    static void close_file();
    static void flush();

    static void trace(const char* fmt, ...);
    static void debug(const char* fmt, ...);
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);

    static void cpu(const char* fmt, ...);

private:
    static LogLevel s_level;
    static bool s_cpu_trace;
    static FILE* s_file;
    static std::string s_file_path;
    static void vlog(LogLevel lvl, const char* prefix, const char* fmt, va_list ap);
};

} // namespace ps96
