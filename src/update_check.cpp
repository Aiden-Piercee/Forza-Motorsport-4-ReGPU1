// Launcher update check: one HTTPS request to the GitHub API for the latest release, read on a
// background thread so the launcher never waits for it. Any failure (offline, no GitHub, a
// different response) leaves the result empty.
#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "version.h"
#include "update_check.h"

#pragma comment(lib, "winhttp.lib")

namespace
{
std::mutex g_resultMutex;
std::wstring g_result;

const wchar_t* kRepoPage = L"github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU/releases";

// The first three numbers of a version such as "v0.2.1-alpha" (0 for any missing one).
void VersionNumbers(const std::string& text, unsigned (&out)[3])
{
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    while (i < text.size() && !isdigit(static_cast<unsigned char>(text[i])))
        i++;
    for (int part = 0; part < 3 && i < text.size(); part++)
    {
        out[part] = unsigned(strtoul(text.c_str() + i, nullptr, 10));
        while (i < text.size() && isdigit(static_cast<unsigned char>(text[i])))
            i++;
        if (i < text.size() && text[i] == '.')
            i++;
    }
}

bool IsNewer(const std::string& remote, const std::string& local)
{
    unsigned r[3], l[3];
    VersionNumbers(remote, r);
    VersionNumbers(local, l);
    for (int i = 0; i < 3; i++)
        if (r[i] != l[i])
            return r[i] > l[i];
    return false;
}

// The "tag_name" value of the release JSON, or empty.
std::string JsonString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos)
        return {};
    const size_t start = at + needle.size();
    const size_t end = json.find('"', start);
    return end == std::string::npos ? std::string() : json.substr(start, end - start);
}

std::string FetchLatestTag()
{
    HINTERNET session = WinHttpOpen(L"FM2-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return {};
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    std::string body;
    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/repos/zer0ducksgiven/Forza-Motorsport-2-ReGPU/releases/latest", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1L), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200)
        {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read))
                    break;
                body.append(chunk.data(), read);
            }
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return JsonString(body, "tag_name");
}
}

void StartUpdateCheck(HWND notify, UINT message)
{
    if (getenv("FM2_NO_UPDATE_CHECK"))
        return;
    std::thread([notify, message] {
        const std::string tag = FetchLatestTag();
        if (!tag.empty() && IsNewer(tag, FM2_VERSION))
        {
            std::wstring text = L"Update available: " + std::wstring(tag.begin(), tag.end()) + L" (" + kRepoPage + L")";
            {
                std::lock_guard lock(g_resultMutex);
                g_result = text;
            }
            PostMessageW(notify, message, 0, 0);
        }
    }).detach();
}

std::wstring UpdateCheckResult()
{
    std::lock_guard lock(g_resultMutex);
    return g_result;
}
