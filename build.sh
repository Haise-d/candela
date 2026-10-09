#!/bin/sh
# Builds candela.exe with MinGW-w64 (on Linux, or MSYS2 on Windows), then runs
# the logic tests natively when a native compiler is around.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build out
$WINDRES res/candela.rc -O coff -o build/res.o
$CXX -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-invalid-offsetof \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -municode -mwindows -Isrc \
  src/*.cpp src/core/*.cpp build/res.o -o out/candela.exe \
  -static -static-libgcc -static-libstdc++ \
  -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lshcore -lole32 -loleaut32 -luuid -lwtsapi32 -lcomctl32 -ltaskschd
x86_64-w64-mingw32-strip out/candela.exe 2>/dev/null || true
echo "built out/candela.exe"
if command -v g++ >/dev/null 2>&1 && [ "$(uname)" = "Linux" ]; then
  g++ -std=c++17 -O1 -Wall -Wno-invalid-offsetof -Isrc/core tests/test_core.cpp src/core/*.cpp -o build/test_core
  TZ=Europe/Madrid build/test_core
fi
