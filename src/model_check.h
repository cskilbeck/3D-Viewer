//////////////////////////////////////////////////////////////////////
// Headless test: load every model in a directory and compare what comes out
// (parts, triangles, materials, textures, size) with <directory>/expected.json
//
//   3D-Viewer --check models            compare, exit code 1 if anything's different
//   3D-Viewer --check models --update   write expected.json from what loads now

#pragma once

#include <filesystem>

int check_models(std::filesystem::path const &directory, bool update);
