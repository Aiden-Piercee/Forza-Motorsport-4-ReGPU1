#pragma once
#include <filesystem>
#include <string>

// Guest path ("game:\media\x.bin", "\Device\Cdrom0\...", symbolic links)
// to host path. readOnly is set for the game folder.
bool VfsResolve(std::string guest, std::filesystem::path& host, bool* readOnly);
void VfsAddLink(const std::string& name, const std::string& target);
void VfsRemoveLink(const std::string& name);
