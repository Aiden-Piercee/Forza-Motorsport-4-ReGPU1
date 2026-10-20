#pragma once
#include <windows.h>

// Asks GitHub for the latest release on a background thread. When it is newer than this build,
// the result text is set and `notify` gets `message` (the launcher then calls UpdateCheckResult).
// Set FM2_NO_UPDATE_CHECK=1 to skip the check.
void StartUpdateCheck(HWND notify, UINT message);

// "Update available: <version> - <page>" when a newer release was found, otherwise empty.
std::wstring UpdateCheckResult();
