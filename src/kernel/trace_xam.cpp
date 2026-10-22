// FM4_TRACE_XAM=1: every XAM, XNotify, XUser, XMsg, XGet and Nui import with its first six arguments and
// what it returns, as "xam: Name(a0..a5) = r3 (thread, lr)". FM4_TRACE_XAM_LIMIT calls per name (300).
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/imports.h"
#include "kernel/thread.h"

bool g_traceXam = getenv("FM4_TRACE_XAM") != nullptr;

namespace
{
struct Call
{
    const char* name;
    uint32_t args[6];
    uint64_t lr;
    bool logged;
};
thread_local std::vector<Call> t_calls; // nested imports (a UI call that completes another)

bool Traced(const char* name)
{
    for (const char* prefix : { "Xam", "XNotify", "XUser", "XMsg", "XGet", "Nui" })
        if (strncmp(name, prefix, strlen(prefix)) == 0)
            return true;
    return false;
}

bool UnderLimit(const char* name)
{
    static const int limit = getenv("FM4_TRACE_XAM_LIMIT") ? atoi(getenv("FM4_TRACE_XAM_LIMIT")) : 300;
    static std::mutex mutex;
    static std::unordered_map<std::string, int> counts;
    std::lock_guard lock(mutex);
    return counts[name]++ < limit;
}
}

void TraceXamBegin(const char* name, PPCContext& ctx)
{
    const bool logged = Traced(name) && UnderLimit(name);
    t_calls.push_back({ name, { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 }, ctx.lr, logged });
}

void TraceXamEnd(const char* name, PPCContext& ctx)
{
    if (t_calls.empty())
        return;
    const Call call = t_calls.back();
    t_calls.pop_back();
    if (!call.logged)
        return;
    const uint32_t* a = call.args;
    Log("xam: %s(%08X %08X %08X %08X %08X %08X) = %08X (thread %u, lr %08llX)", name, a[0], a[1], a[2], a[3], a[4], a[5], ctx.r3.u32,
        CurrentGuestThreadId(), call.lr);
}
