#pragma once
#include <filesystem>

// Documents\fm2 in the Documents folder Windows reports (it may have been moved off C:), see user_dir.cpp.
const std::filesystem::path& DocumentsUserDir();
