#!/bin/sh
# Build the VPK inside the VitaSDK Docker image. Run from the project folder.
set -e
cd "$(dirname "$0")"
python3 tools/gen_import_table.py imports.txt src/import_table.c
rm -rf build
# LOADER_DEBUG=ON ./build.sh   -> verbose diagnostics in log.txt (default OFF)
docker run --rm -e LOADER_DEBUG="${LOADER_DEBUG:-OFF}" -v "$PWD:/workspace" -w /workspace vitasdk/vitasdk:latest sh -c '
  vdpm vitaGL sdl2_vitagl kubridge &&
  git clone https://github.com/Rinnegatamante/vitaGL.git /tmp/vitaGL &&
  git -C /tmp/vitaGL checkout cd3791e &&
  make -C /tmp/vitaGL -j4 NO_SPLASHSCREEN=1 install &&
  mkdir -p build && cd build &&
  cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DLOADER_DEBUG=$LOADER_DEBUG &&
  make 2>&1 | tail -60'
