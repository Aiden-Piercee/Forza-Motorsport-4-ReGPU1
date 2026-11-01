#include "log.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::mutex g_logMutex;
std::condition_variable g_logCv, g_logDrained;
std::deque<std::string> g_queue; // lines waiting for the writer thread (so a slow disk never blocks the game)
bool g_writerBusy = false;
bool g_writerStarted = false;
FILE* g_logFile = nullptr;
const auto g_start = std::chrono::steady_clock::now();
std::deque<std::string> g_recent; // the last lines, for the debug server's log command

// REGPU_LOG_DIR: where the log file goes (a slow or removable drive stalled game threads on every write).
std::string LogPath(const char* name)
{
    const char* dir = getenv("REGPU_LOG_DIR");
    return dir && *dir ? std::string(dir) + char(92) + name : std::string(name);
}

// Writes queued lines in batches, one flush per batch; a batch that takes over 200 ms is reported (a stalling drive froze
// every thread that logged when each line was flushed by the thread that logged it).
void WriterLoop()
{
    std::deque<std::string> batch;
    for (;;)
    {
        {
            std::unique_lock lock(g_logMutex);
            g_writerBusy = false;
            g_logDrained.notify_all();
            g_logCv.wait(lock, [] { return !g_queue.empty(); });
            g_writerBusy = true;
            batch.swap(g_queue);
        }
        const auto start = std::chrono::steady_clock::now();
        if (!g_logFile)
            g_logFile = fopen(LogPath("fm2.log").c_str(), "w");
        for (const std::string& line : batch)
            for (FILE* f : { stderr, g_logFile })
                if (f)
                    fputs(line.c_str(), f);
        for (FILE* f : { stderr, g_logFile })
            if (f)
                fflush(f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const size_t lines = batch.size();
        batch.clear();
        if (ms > 200.0)
        {
            char note[160];
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
            snprintf(note, sizeof(note), "[%9.4f] log: writing %zu lines took %.0f ms (slow disk?)%c", t, lines, ms, 10);
            std::lock_guard lock(g_logMutex);
            g_queue.emplace_back(note);
        }
    }
}

void Write(const char* prefix, const char* fmt, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();

    char line[2120];
    snprintf(line, sizeof(line), "[%9.4f] %s%s", t, prefix, buffer);
    std::lock_guard lock(g_logMutex);
    g_recent.emplace_back(line);
    if (g_recent.size() > 2000)
        g_recent.pop_front();
    if (!g_writerStarted)
    {
        g_writerStarted = true;
        std::thread(WriterLoop).detach();
        atexit([] { LogFlush(); });
    }
    if (g_queue.size() < 200000)
        g_queue.emplace_back(std::string(line) + char(10));
    g_logCv.notify_one();
}
}

void LogFlush()
{
    std::unique_lock lock(g_logMutex);
    if (!g_writerStarted)
        return;
    g_logDrained.wait_for(lock, std::chrono::seconds(5), [] { return g_queue.empty() && !g_writerBusy; });
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
    LogFlush();
    std::_Exit(1);
}

std::vector<std::string> LogTail(size_t n)
{
    std::lock_guard lock(g_logMutex);
    const size_t from = g_recent.size() > n ? g_recent.size() - n : 0;
    return std::vector<std::string>(g_recent.begin() + ptrdiff_t(from), g_recent.end());
}
