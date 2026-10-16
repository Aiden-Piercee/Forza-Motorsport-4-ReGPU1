#pragma once
#include <filesystem>

// Starts the runtime services the title expects from the console: the
// timestamp bundle, the file system roots, the fake GPU and audio clocks.
void RuntimeInit(const std::filesystem::path& gameDir);

const std::filesystem::path& GameDir();
const std::filesystem::path& UserDir();
