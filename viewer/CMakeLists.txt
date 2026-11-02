# Standalone WebGL viewers — WASM module build.
#
# Configure with the Emscripten toolchain via emcmake, e.g.:
#   emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Three independent modules, copied to js/: ssv_wasm (index.html),
# ssv_telemetry (telemetry.html) and ss_select (no page; the frame selection a
# web app that decodes video itself runs); editing one rebuilds only that one.
#
# ssv_wasm links two kinds of native code:
#   1. src/viewer.cpp — dependency-free, performance-critical PLY/OBJ parsing,
#      depth sorting, and statistics (kept -ffast-math / -fno-exceptions).
#   2. The trainer's dataset parsers (COLMAP / Nerfstudio / Metashape), reused
#      IN-PLACE from ../src/data (referenced by relative path, not copied)
#      and driven from src/dataset_bridge.cpp over MEMFS.
# GLTF/GLB are parsed in JavaScript.

cmake_minimum_required(VERSION 3.16)
project(ssv_viewer_wasm CXX C)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "This project must be configured with emcmake (Emscripten toolchain).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Reused trainer sources (in-place; carried in the same git repo).
set(SS_SRC "${CMAKE_SOURCE_DIR}/../src")

# The parsers report through i18n::format, so the module needs its runtime
# (src/i18n/). Globbed, to stay in step with cmake/SsI18n.cmake.
file(GLOB I18N_SOURCES CONFIGURE_DEPENDS "${SS_SRC}/i18n/*.cpp")

set(DATASET_SOURCES
  "${CMAKE_SOURCE_DIR}/src/dataset_bridge.cpp"
  "${SS_SRC}/data/parsers/ColmapParser.cpp"
  "${SS_SRC}/data/parsers/NerfstudioParser.cpp"
  "${SS_SRC}/data/parsers/MetashapeParser.cpp"
  "${SS_SRC}/data/parsers/DatasetCommon.cpp"
  "${SS_SRC}/data/SourceCamera.cpp"
  "${SS_SRC}/data/DistortionFit.cpp"
  ${I18N_SOURCES}
  "${SS_SRC}/external/miniz.c"
)

add_executable(ssv_wasm src/viewer.cpp ${DATASET_SOURCES})

# src/ is the trainer tree's include root: dataset_bridge.cpp includes
# data/DatasetParser.h, data/Json.h, data/Xml.h and the parser sources
# path-qualify their own includes the same way.
target_include_directories(ssv_wasm PRIVATE "${SS_SRC}")

# The trainer tree's SS_FILE contract (src/core/SourcePath.h): the shared
# sources must report the same repo-relative paths built from here.
get_filename_component(SS_ROOT "${SS_SRC}/.." ABSOLUTE)
target_compile_definitions(ssv_wasm PRIVATE SS_SOURCE_ROOT="${SS_ROOT}")

# Per-source flags: viewer.cpp stays fast/exception-free; the dataset parsers
# need C++ exceptions (they throw on bad input) and must NOT use -ffast-math
# (they rely on inf/NaN semantics, e.g. outlier_threshold = infinity()).
set_source_files_properties(src/viewer.cpp PROPERTIES
  COMPILE_OPTIONS "-O3;-ffast-math;-fno-exceptions;-fno-rtti")
set_source_files_properties(${DATASET_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-O3;-fexceptions")

# Emscripten link options.
#  - MODULARIZE + EXPORT_NAME: load as a factory function (ES module friendly).
#  - ALLOW_MEMORY_GROWTH + MAXIMUM_MEMORY: support very large (multi-GB) models.
#  - FORCE_FILESYSTEM + FS: mount dropped dataset files into MEMFS.
#  - -fexceptions: enable exception support for the dataset parsers.
#  - EXPORTED_RUNTIME_METHODS: cwrap/ccall + heap views + FS used from JS.
#  - The exported C functions are auto-kept via EMSCRIPTEN_KEEPALIVE.
target_link_options(ssv_wasm PRIVATE
  -O3
  -fexceptions
  "-sMODULARIZE=1"
  "-sEXPORT_NAME=SSVModule"
  "-sEXPORT_ES6=1"
  "-sENVIRONMENT=web,worker"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sMAXIMUM_MEMORY=4gb"
  "-sINITIAL_MEMORY=64mb"
  "-sSTACK_SIZE=1mb"
  "-sALLOW_TABLE_GROWTH=1"
  "-sFORCE_FILESYSTEM=1"
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','HEAPU8','HEAPU16','HEAPU32','HEAPF32','HEAP32']"
)

set_target_properties(ssv_wasm PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME "ssv_wasm"
  SUFFIX ".js"
)

# Copy build artifacts next to the JS sources so index.html can load them.
add_custom_command(TARGET ssv_wasm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ssv_wasm.js" "${CMAKE_SOURCE_DIR}/js/ssv_wasm.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ssv_wasm.wasm" "${CMAKE_SOURCE_DIR}/js/ssv_wasm.wasm"
  COMMENT "Copying WASM artifacts into js/"
)

# ---------------------------------------------------------------------------
# Second module, for telemetry.html: the trainer's IMU/GPS reader
# (src/sfm/core/Telemetry.cpp, in place) behind a C ABI. Separate from
# ssv_wasm on purpose -- editing one viewer's sources rebuilds only it, and
# neither page pays for the other's code.
# ---------------------------------------------------------------------------
add_executable(ssv_telemetry src/telemetry_bridge.cpp "${SS_SRC}/sfm/core/Telemetry.cpp")
target_include_directories(ssv_telemetry PRIVATE "${SS_SRC}")
target_compile_definitions(ssv_telemetry PRIVATE SS_SOURCE_ROOT="${SS_ROOT}")
set_source_files_properties(src/telemetry_bridge.cpp "${SS_SRC}/sfm/core/Telemetry.cpp"
  PROPERTIES COMPILE_OPTIONS "-O3;-fno-exceptions;-fno-rtti")

# No filesystem and no exceptions: the parser is fed bytes through a callback
# the worker answers from a File slice, so a multi-GB capture never lands in
# the heap.
target_link_options(ssv_telemetry PRIVATE
  -O3
  "-sMODULARIZE=1"
  "-sEXPORT_NAME=SSTModule"
  "-sEXPORT_ES6=1"
  "-sENVIRONMENT=web,worker"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sMAXIMUM_MEMORY=2gb"
  "-sINITIAL_MEMORY=32mb"
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPU8','HEAPF32','HEAPF64']"
)

set_target_properties(ssv_telemetry PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME "ssv_telemetry"
  SUFFIX ".js"
)

add_custom_command(TARGET ssv_telemetry POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ssv_telemetry.js" "${CMAKE_SOURCE_DIR}/js/ssv_telemetry.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ssv_telemetry.wasm" "${CMAKE_SOURCE_DIR}/js/ssv_telemetry.wasm"
  COMMENT "Copying telemetry WASM artifacts into js/"
)

# ---------------------------------------------------------------------------
# Third module: the engine's frame selection (src/app's FrameMotion.cpp,
# FrameSharpness.cpp, SelectClock.h and FramePick.h, in place) behind the C ABI
# of src/select_bridge.cpp, typed by js/ss_select.d.ts, for a browser that
# decodes a video itself and has to keep the frames `spirula sam extract` keeps.
# ---------------------------------------------------------------------------
set(SELECT_SOURCES
  "${CMAKE_SOURCE_DIR}/src/select_bridge.cpp"
  "${SS_SRC}/app/FrameMotion.cpp"
  "${SS_SRC}/app/FrameSharpness.cpp"
  "${SS_SRC}/app/Pano360.cpp"
)
add_executable(ss_select ${SELECT_SOURCES})
target_include_directories(ss_select PRIVATE "${SS_SRC}")
target_compile_definitions(ss_select PRIVATE SS_SOURCE_ROOT="${SS_ROOT}")
# No -ffast-math: the plan has to round as the native build does, and
# test/select.test.mjs holds it to the expected file the native test checks.
set_source_files_properties(${SELECT_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-O3;-fno-exceptions;-fno-rtti")

target_link_options(ss_select PRIVATE
  -O3
  "-sMODULARIZE=1"
  "-sEXPORT_NAME=SSSelectModule"
  "-sEXPORT_ES6=1"
  "-sENVIRONMENT=web,worker"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sMAXIMUM_MEMORY=2gb"
  "-sINITIAL_MEMORY=32mb"
  "-sFILESYSTEM=0"
  "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP32','HEAPU32','HEAPF32','HEAPF64']"
)

set_target_properties(ss_select PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME "ss_select"
  SUFFIX ".js"
)

add_custom_command(TARGET ss_select POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ss_select.js" "${CMAKE_SOURCE_DIR}/js/ss_select.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/ss_select.wasm" "${CMAKE_SOURCE_DIR}/js/ss_select.wasm"
  COMMENT "Copying selection WASM artifacts into js/"
)

# Last: makes every module depend on the repo's source lints (docs/build.md).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SsChecks.cmake)
