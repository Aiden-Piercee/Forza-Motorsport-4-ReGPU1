#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

// Thread-safe log to stderr and fm2.log next to the exe.
void Log(const char* fmt, ...);
[[noreturn]] void Fatal(const char* fmt, ...);
// The last n lines (up to 2000 are kept), for the debug server.
std::vector<std::string> LogTail(size_t n);

#define LOG_ONCE(...)                \
    do                               \
    {                                \
        static bool logged_ = false; \
        if (!logged_)                \
        {                            \
            logged_ = true;          \
            Log(__VA_ARGS__);        \
        }                            \
    } while (0)
