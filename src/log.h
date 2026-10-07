//////////////////////////////////////////////////////////////////////
// Logging - same shape as gerber_lib's gerber_log but standalone

#pragma once

#include <format>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

//////////////////////////////////////////////////////////////////////
// DebugBreak

#if defined(_MSC_VER)
#define DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#if defined(__i386__) || defined(__x86_64__)
#define DEBUG_BREAK() __asm__ volatile("int $3")
#elif defined(__arm64__) || defined(__aarch64__)
#define DEBUG_BREAK() __asm__ volatile("brk #0")
#elif defined(__arm__)
#define DEBUG_BREAK() __asm__ volatile("bkpt #0")
#else
#include <signal.h>
#define DEBUG_BREAK() raise(SIGTRAP)
#endif
#else
#include <signal.h>
#define DEBUG_BREAK() raise(SIGTRAP)
#endif

//////////////////////////////////////////////////////////////////////

inline bool is_debugger_present()
{
#if defined(_WIN32)
    return IsDebuggerPresent() != 0;

#elif defined(__APPLE__)
    // Checks the kinfo_proc flags for the P_TRACED bit
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    struct kinfo_proc info;
    size_t size = sizeof(info);
    info.kp_proc.p_flag = 0;
    if(sysctl(mib, 4, &info, &size, nullptr, 0) == 0) {
        return (info.kp_proc.p_flag & P_TRACED) != 0;
    }
    return false;

#elif defined(__linux__)
    // TracerPid in /proc/self/status is non-zero if being debugged
    std::ifstream infile("/proc/self/status");
    std::string line;
    while(std::getline(infile, line)) {
        if(line.find("TracerPid:") == 0) {
            return std::stoi(line.substr(10)) != 0;
        }
    }
    return false;
#else
    return false;
#endif
}

#define SAFE_TRAP()                 \
    do {                            \
        if(is_debugger_present()) { \
            DEBUG_BREAK();          \
        }                           \
    } while(0)

//////////////////////////////////////////////////////////////////////

namespace logging
{
    enum log_level_t
    {
        log_level_debug = 0,
        log_level_verbose = 1,
        log_level_info = 2,
        log_level_warning = 3,
        log_level_error = 4,
        log_level_fatal = 5,
        log_level_none = 6
    };

    //////////////////////////////////////////////////////////////////////

    struct log_context
    {
        char const *context;
        log_level_t max_level;
    };

    //////////////////////////////////////////////////////////////////////

    typedef int (*log_emitter_function_t)(char const *);

    extern log_level_t log_level;

    extern log_emitter_function_t log_emitter_function;

    //////////////////////////////////////////////////////////////////////

    inline void log_set_level(log_level_t level)
    {
        log_level = level;
    }

    //////////////////////////////////////////////////////////////////////

    inline void log_set_emitter_function(log_emitter_function_t function)
    {
        log_emitter_function = function;
    }

    //////////////////////////////////////////////////////////////////////

    void emit(log_level_t level, char const *context, char const *fmt, std::format_args const &fmt_args);

    //////////////////////////////////////////////////////////////////////

    template <typename... args> constexpr void log(log_level_t level, log_context const &context, char const *fmt, args &&...arguments)
    {
        if(level == log_level_fatal || (level >= log_level && level >= context.max_level)) {
            emit(level, context.context, fmt, std::make_format_args(arguments...));
        }
        if(level == log_level_fatal) {
            SAFE_TRAP();
        }
    }

}    // namespace logging

//////////////////////////////////////////////////////////////////////

#define LOG_CONTEXT(context, max_level)                                     \
    [[maybe_unused]] static constexpr ::logging::log_context __log_context \
    {                                                                       \
        context, ::logging::log_level_t::log_level_##max_level              \
    }

#define LOG_DEBUG(msg, ...) ::logging::log(::logging::log_level_debug, __log_context, msg, ##__VA_ARGS__)
#define LOG_VERBOSE(msg, ...) ::logging::log(::logging::log_level_verbose, __log_context, msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) ::logging::log(::logging::log_level_info, __log_context, msg, ##__VA_ARGS__)
#define LOG_WARNING(msg, ...) ::logging::log(::logging::log_level_warning, __log_context, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) ::logging::log(::logging::log_level_error, __log_context, msg, ##__VA_ARGS__)
#define LOG_FATAL(msg, ...) ::logging::log(::logging::log_level_fatal, __log_context, msg, ##__VA_ARGS__)

#define LOG_ASSERT(x)                                                                \
    do                                                                               \
        if(!(x))                                                                     \
            LOG_FATAL("ASSERT FAILED: {} at line {} of {}", #x, __LINE__, __FILE__); \
    while(false)
