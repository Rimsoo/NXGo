// NXGo: route KataGo's ghc::filesystem to libstdc++'s std::filesystem (devkitA64 newlib).
#pragma once
#include <filesystem>
#include <fstream>
namespace ghc { namespace filesystem {
  using namespace std::filesystem;
  using std::ifstream; using std::ofstream; using std::fstream;
} }
