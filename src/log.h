#pragma once
#include <cstdarg>
#include <cstdio>

// Thread-safe log to stderr and fm2.log next to the exe.
void Log(const char* fmt, ...);
[[noreturn]] void Fatal(const char* fmt, ...);

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
