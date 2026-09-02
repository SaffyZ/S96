#include "common/Log.hpp"
#include <mutex>

namespace ps96 {

LogLevel Log::s_level = LogLevel::Warn;
bool Log::s_cpu_trace = false;
FILE* Log::s_file = nullptr;
std::string Log::s_file_path;
static std::mutex s_log_mutex;

void Log::set_level(LogLevel level) { s_level = level; }
LogLevel Log::level() { return s_level; }
void Log::set_cpu_trace(bool enabled) { s_cpu_trace = enabled; }
bool Log::cpu_trace() { return s_cpu_trace; }

bool Log::open_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    if (s_file) {
        std::fclose(s_file);
        s_file = nullptr;
    }
    s_file = std::fopen(path.c_str(), "wb");
    if (!s_file) return false;
    s_file_path = path;
    std::setvbuf(s_file, nullptr, _IOLBF, 0);
    std::fprintf(s_file, "PS96 debug log\n");
    return true;
}

void Log::close_file() {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    if (s_file) {
        std::fflush(s_file);
        std::fclose(s_file);
        s_file = nullptr;
    }
}

void Log::flush() {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    std::fflush(stderr);
    if (s_file) std::fflush(s_file);
}

void Log::vlog(LogLevel lvl, const char* prefix, const char* fmt, va_list ap) {
    if (static_cast<int>(lvl) < static_cast<int>(s_level)) return;
    std::lock_guard<std::mutex> lock(s_log_mutex);

    va_list copy;
    va_copy(copy, ap);
    std::fputs(prefix, stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);

    if (s_file) {
        std::fputs(prefix, s_file);
        std::vfprintf(s_file, fmt, copy);
        std::fputc('\n', s_file);
        std::fflush(s_file);
    }
    va_end(copy);
}

void Log::trace(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(LogLevel::Trace, "[TRACE] ", fmt, ap); va_end(ap);
}
void Log::debug(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(LogLevel::Debug, "[DEBUG] ", fmt, ap); va_end(ap);
}
void Log::info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(LogLevel::Info,  "[INFO]  ", fmt, ap); va_end(ap);
}
void Log::warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(LogLevel::Warn,  "[WARN]  ", fmt, ap); va_end(ap);
}
void Log::error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(LogLevel::Error, "[ERROR] ", fmt, ap); va_end(ap);
}
void Log::cpu(const char* fmt, ...) {
    if (!s_cpu_trace) return;
    va_list ap; va_start(ap, fmt);
    std::lock_guard<std::mutex> lock(s_log_mutex);
    va_list copy;
    va_copy(copy, ap);
    std::fputs("[CPU] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    if (s_file) {
        std::fputs("[CPU] ", s_file);
        std::vfprintf(s_file, fmt, copy);
        std::fputc('\n', s_file);
        std::fflush(s_file);
    }
    va_end(copy);
    va_end(ap);
}

} // namespace ps96
