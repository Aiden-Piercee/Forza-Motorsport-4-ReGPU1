#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace regpu
{
// Thread-safe log to stderr and regpu.log in the working folder.
void Log(const char* fmt, ...);
// Waits (up to 2 s) until queued lines are written: before the process dies.
void LogFlush();
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
} // namespace regpu
