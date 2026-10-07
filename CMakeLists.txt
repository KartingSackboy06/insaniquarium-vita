cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not defined")
  endif()
endif()
project(InsaniquariumVita C)

# One version number for everything: the VPK (LiveArea / Vita settings) and the first line of log.txt.
set(LOADER_VERSION "01.00")
# Verbose diagnostics in log.txt (per-second video status, memory/fps heartbeat, file-open trace, pad events).
# Off for releases; build.sh passes it through:  LOADER_DEBUG=ON ./build.sh
option(LOADER_DEBUG "Verbose diagnostics in log.txt" OFF)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -g -mfpu=neon -Wall -Wno-unused-function -std=gnu11")
# Route the unwinder's exidx lookup through src/unwind_hook.c (C++ exceptions inside libmain.so).
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--wrap,__gnu_Unwind_Find_exidx")

add_executable(InsaniquariumVita
  src/main.c
  src/so_util.c
  src/unwind_hook.c
  src/wrappers.c
  src/gl_wrappers.c
  src/intro_video.c
  src/import_table.c
)

target_compile_definitions(InsaniquariumVita PRIVATE LOADER_VERSION="${LOADER_VERSION}")
if(LOADER_DEBUG)
  target_compile_definitions(InsaniquariumVita PRIVATE LOADER_DEBUG=1)
endif()

target_link_libraries(InsaniquariumVita
  SDL2
  vitaGL
  vitashark
  mathneon
  SceShaccCgExt
  taihen_stub
  kubridge_stub
  SceShaccCg_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceAvPlayer_stub
  SceAudioIn_stub
  SceMotion_stub
  SceIme_stub
  SceHid_stub
  SceAppMgr_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  ScePgf_stub
  SceFiber_stub
  SceKernelDmacMgr_stub
  ScePower_stub
  SceSysmem_stub
  pthread
  stdc++
  m
  c
)

# UNSAFE: required so the eboot may use kubridge to map executable memory.
vita_create_self(eboot.bin InsaniquariumVita UNSAFE)
# Extended memory mode: 365 MiB of app memory instead of 256 MiB.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_vpk(InsaniquariumVita.vpk INSNQ4R13 eboot.bin
  VERSION "${LOADER_VERSION}"
  NAME "Insaniquarium!"
  FILE
    ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    ${CMAKE_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
    ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
    ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    ${CMAKE_SOURCE_DIR}/USRDIR/movies/popcap_logo.mp4 USRDIR/movies/popcap_logo.mp4
    ${CMAKE_SOURCE_DIR}/USRDIR/movies/soe_logo.mp4 USRDIR/movies/soe_logo.mp4
    ${CMAKE_SOURCE_DIR}/libmain.so libmain.so
)
