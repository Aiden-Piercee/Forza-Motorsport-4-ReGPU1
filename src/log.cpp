#include "log.h"

#include <chrono>
#include <cstdlib>
#include <mutex>

namespace
{
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
const auto g_start = std::chrono::steady_clock::now();

void Write(const char* prefix, const char* fmt, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();

    std::lock_guard lock(g_logMutex);
    if (!g_logFile)
        g_logFile = fopen("fm2.log", "w");
    for (FILE* f : { stderr, g_logFile })
    {
        if (f)
        {
            fprintf(f, "[%9.4f] %s%s\n", t, prefix, buffer);
            fflush(f);
        }
    }
}
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("", fmt, args);
    va_end(args);
}

void Fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("FATAL: ", fmt, args);
    va_end(args);
    std::_Exit(1);
}
