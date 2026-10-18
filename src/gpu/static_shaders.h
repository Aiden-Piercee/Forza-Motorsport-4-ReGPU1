#pragma once
#include <vector>

#include "gpu/shader_scan.h"

// Every shader in the game data and the executable, found by structure. The first call
// scans (about half a minute) and saves the list; later calls load it.
std::vector<ScannedShader> StaticShaders();
