cmake_minimum_required(VERSION 3.29)

project(Tox2Recomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerChecks.cmake)
include(cmake/PpuCodegen.cmake)
include(cmake/SpuCodegen.cmake)
include(cmake/Runtime.cmake)
include(cmake/Render.cmake)
include(cmake/Overlay.cmake)
include(cmake/Media.cmake)
include(cmake/Audio.cmake)
include(cmake/Input.cmake)
include(cmake/Decrypt.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_executable(tox2_runtime runtime/src/main.cpp)
target_compile_definitions(tox2_runtime PRIVATE TOX2_VERSION="${PROJECT_VERSION}")
target_compile_options(tox2_runtime PRIVATE -march=x86-64-v3 -Wall -Wextra)
target_include_directories(tox2_runtime PRIVATE runtime/include)

add_library(tox2_ppu_stub STATIC runtime/src/ppu_stub_runtime.cpp)
target_compile_options(tox2_ppu_stub PRIVATE -march=x86-64-v3 -Wall -Wextra)
target_include_directories(tox2_ppu_stub PUBLIC runtime/include)

add_library(tox2_spu_stub STATIC runtime/src/spu_stub_runtime.cpp)
target_compile_options(tox2_spu_stub PRIVATE -march=x86-64-v3 -Wall -Wextra)
target_include_directories(tox2_spu_stub PUBLIC runtime/include)

set(TOX2_EBOOT_ELF "" CACHE FILEPATH "Decrypted EBOOT.elf for optional tests")
# The file tox2_inputs produces; a build step, so it need not exist yet.
if(TOX2_EBOOT_ELF STREQUAL "" AND NOT TOX2_EBOOT_BIN STREQUAL "")
  set_property(CACHE TOX2_EBOOT_ELF PROPERTY VALUE "${TOX2_INPUTS_DIR}/EBOOT.elf")
endif()

tox2_ppu_add_generated_tree()
tox2_spu_add_generated_tree()
tox2_runtime_add_core()
tox2_decrypt_add()
tox2_media_add()
tox2_audio_add()
tox2_input_add()
tox2_render_add()
tox2_overlay_add()
if(TARGET tox2_game)
  # tox2_media supplies the FFmpeg codec factory that game_main installs for the cellSail movies; the
  # FFmpeg DLLs it loads at run time are copied next to the executable by tox2_ffmpeg_runtime.
  target_link_libraries(tox2_game PRIVATE tox2_render tox2_overlay tox2_media)
  add_dependencies(tox2_game tox2_ffmpeg_runtime)
endif()

add_executable(tox2_ppu_header_check runtime/tests/ppu_header_check.c)
tox2_ppu_codegen_options(tox2_ppu_header_check)
target_link_libraries(tox2_ppu_header_check PRIVATE tox2_ppu_stub)

add_test(NAME runtime_version COMMAND tox2_runtime --version)
set_tests_properties(runtime_version PROPERTIES
  PASS_REGULAR_EXPRESSION "tox2_runtime 0\\.1\\.0")

add_test(NAME runtime_musttail COMMAND tox2_runtime --selftest-musttail)
set_tests_properties(runtime_musttail PROPERTIES
  PASS_REGULAR_EXPRESSION "musttail ok")

add_test(NAME ppu_header_check COMMAND tox2_ppu_header_check)
set_tests_properties(ppu_header_check PROPERTIES
  PASS_REGULAR_EXPRESSION "ppu header ok")

tox2_spu_add_header_check()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
tox2_ppu_add_semantics_harness()
tox2_spu_add_semantics_harness()
tox2_spu_add_programs()
tox2_spu_add_lzma_inflate_test()

add_test(NAME tools_unittest
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s tools/tests -t tools
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# The relocated libsre exists only when the build produces it from a firmware update.
set(TOX2_LIBSRE_ELF "")
if(NOT TOX2_PUP STREQUAL "")
  set(TOX2_LIBSRE_ELF "${TOX2_INPUTS_DIR}/libsre.reloc.elf")
endif()
set_tests_properties(tools_unittest PROPERTIES
  ENVIRONMENT "TOX2_EBOOT_ELF=${TOX2_EBOOT_ELF};TOX2_LIBSRE_ELF=${TOX2_LIBSRE_ELF}")
