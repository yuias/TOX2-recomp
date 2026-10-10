cmake_minimum_required(VERSION 3.29)

project(DalRinneUtopiaRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerChecks.cmake)
include(cmake/PpuCodegen.cmake)
include(cmake/Runtime.cmake)
include(cmake/Render.cmake)
include(cmake/Media.cmake)
include(cmake/Audio.cmake)
include(cmake/Input.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_executable(dal_runtime runtime/src/main.cpp)
target_compile_definitions(dal_runtime PRIVATE DAL_VERSION="${PROJECT_VERSION}")
target_compile_options(dal_runtime PRIVATE -march=x86-64-v3 -Wall -Wextra)
target_include_directories(dal_runtime PRIVATE runtime/include)

add_library(dal_ppu_stub STATIC runtime/src/ppu_stub_runtime.cpp)
target_compile_options(dal_ppu_stub PRIVATE -march=x86-64-v3 -Wall -Wextra)
target_include_directories(dal_ppu_stub PUBLIC runtime/include)

set(DAL_EBOOT_ELF "" CACHE FILEPATH "Decrypted EBOOT.elf for optional tests")

dal_ppu_add_generated_tree()
dal_runtime_add_core()
dal_media_add()
dal_audio_add()
dal_input_add()
dal_render_add()
if(TARGET dal_game)
  # dal_media supplies the FFmpeg codec factory that game_main installs for the cellSail movies; the
  # FFmpeg DLLs it loads at run time are copied next to the executable by dal_ffmpeg_runtime.
  target_link_libraries(dal_game PRIVATE dal_render dal_media)
  add_dependencies(dal_game dal_ffmpeg_runtime)
endif()

add_executable(dal_ppu_header_check runtime/tests/ppu_header_check.c)
dal_ppu_codegen_options(dal_ppu_header_check)
target_link_libraries(dal_ppu_header_check PRIVATE dal_ppu_stub)

add_test(NAME runtime_version COMMAND dal_runtime --version)
set_tests_properties(runtime_version PROPERTIES
  PASS_REGULAR_EXPRESSION "dal_runtime 0\\.1\\.0")

add_test(NAME runtime_musttail COMMAND dal_runtime --selftest-musttail)
set_tests_properties(runtime_musttail PROPERTIES
  PASS_REGULAR_EXPRESSION "musttail ok")

add_test(NAME ppu_header_check COMMAND dal_ppu_header_check)
set_tests_properties(ppu_header_check PROPERTIES
  PASS_REGULAR_EXPRESSION "ppu header ok")

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
dal_ppu_add_semantics_harness()

add_test(NAME tools_unittest
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s tools/tests -t tools
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(tools_unittest PROPERTIES
  ENVIRONMENT "DAL_EBOOT_ELF=${DAL_EBOOT_ELF}")
