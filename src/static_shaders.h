#pragma once
#include <vector>

#include "regpu/regpu.h"

// Every shader in the game data and the executable, found by structure (regpu::Host::staticShaders).
// The first call searches (a few seconds) and saves the list; later calls load it.
std::vector<regpu::ScannedShader> StaticShaders();
// Whether the list is already saved (regpu::Host::staticShadersSaved).
bool StaticShadersSaved();
