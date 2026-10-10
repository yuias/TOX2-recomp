include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/gameinput.cmake")

# The Microsoft.GameInput package is fetched and verified by tools/fetch-gameinput.ps1 into
# generated/deps/gameinput. Needs a successful fetch, which leaves a marker holding the SHA-256 of
# the pinned .nupkg.
#
# Sets in the caller's scope:
#   DAL_GAMEINPUT_INCLUDE_DIR  the v3 header directory. Targets must add it as a SYSTEM include:
#                              the Windows SDK ships an older GameInput.h on the default include path.
#   DAL_GAMEINPUT_LIB          GameInput.lib, a static loader that resolves the runtime with
#                              LoadLibrary (it imports nothing from a GameInput DLL). Link it by this
#                              full path: the SDK's own gameinput.lib is an import library.
#
# The runtime is not part of the build: the user installs redist/GameInputRedist.msi, which the
# README points to (checked here so a broken extraction is caught). A copy of its DLL next to the
# executable would take precedence over the installed one and hide Xbox pads.
function(dal_input_find_gameinput)
  set(DAL_GAMEINPUT_DIR "" CACHE PATH
    "Extracted Microsoft.GameInput package (default: generated/deps/gameinput/<pinned version>)")
  if("${DAL_GAMEINPUT_DIR}" STREQUAL "")
    set(gameinput_dir "${CMAKE_SOURCE_DIR}/generated/deps/gameinput/${DAL_GAMEINPUT_VERSION}")
  else()
    set(gameinput_dir "${DAL_GAMEINPUT_DIR}")
  endif()

  set(marker "${gameinput_dir}/.verified-sha256")
  # A fetch after this configure must re-run it.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
  set(fetch_hint "Run: pwsh -NoProfile -File tools/fetch-gameinput.ps1")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "DAL: GameInput is not fetched (${marker} not found). ${fetch_hint}")
  endif()
  file(READ "${marker}" marker_hash)
  string(STRIP "${marker_hash}" marker_hash)
  string(TOLOWER "${marker_hash}" marker_hash)
  string(TOLOWER "${DAL_GAMEINPUT_SHA256}" pinned_hash)
  if(NOT marker_hash STREQUAL pinned_hash)
    message(FATAL_ERROR
      "DAL: GameInput in ${gameinput_dir} does not match the pin in cmake/gameinput.cmake. ${fetch_hint}")
  endif()
  foreach(path native/include/GameInput.h native/lib/x64/GameInput.lib redist/GameInputRedist.msi)
    if(NOT EXISTS "${gameinput_dir}/${path}")
      message(FATAL_ERROR "DAL: ${gameinput_dir}/${path} is missing. ${fetch_hint} -Force")
    endif()
  endforeach()

  set(DAL_GAMEINPUT_INCLUDE_DIR "${gameinput_dir}/native/include" PARENT_SCOPE)
  set(DAL_GAMEINPUT_LIB "${gameinput_dir}/native/lib/x64/GameInput.lib" PARENT_SCOPE)

  # Relative to the source tree so the log carries no machine-specific prefix.
  file(RELATIVE_PATH gameinput_rel "${CMAKE_SOURCE_DIR}" "${gameinput_dir}")
  message(STATUS "DAL: GameInput: ${gameinput_rel}")
endfunction()

# The input libraries and tools. Call after dal_runtime_add_core() and enable_testing().
function(dal_input_add)
  dal_input_find_gameinput()

  # The only target that sees GameInput.h and Windows.h for gamepads; dal_runtime_core stays free of
  # them. The v3 header directory is a SYSTEM include so it is searched before the Windows SDK's
  # older GameInput.h. GameInput.lib is the package's static loader (it resolves the installed
  # runtime with LoadLibrary), so no GameInput DLL appears in an import table.
  add_library(dal_input_gameinput STATIC runtime/src/input/gameinput_backend.cpp)
  target_compile_options(dal_input_gameinput PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_input_gameinput SYSTEM PRIVATE "${DAL_GAMEINPUT_INCLUDE_DIR}")
  target_link_libraries(dal_input_gameinput PUBLIC dal_runtime_core PRIVATE "${DAL_GAMEINPUT_LIB}")

  # Pure core code behind the GamepadBackend interface; no GameInput or Windows.
  add_executable(dal_input_core_tests runtime/tests/input_core_test.cpp)
  target_compile_options(dal_input_core_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_input_core_tests PRIVATE dal_runtime_core)

  add_test(NAME input_core COMMAND dal_input_core_tests)
  set_tests_properties(input_core PROPERTIES
    PASS_REGULAR_EXPRESSION "input core tests: [0-9]+ passed, 0 failed")

  # Exits 77 (skipped) when no GameInput runtime with API version 3 is available.
  add_executable(dal_input_gameinput_tests runtime/tests/input_gameinput_test.cpp)
  target_compile_options(dal_input_gameinput_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_input_gameinput_tests PRIVATE dal_input_gameinput)

  add_test(NAME input_gameinput COMMAND dal_input_gameinput_tests)
  set_tests_properties(input_gameinput PROPERTIES
    SKIP_RETURN_CODE 77
    PASS_REGULAR_EXPRESSION "input gameinput tests: [0-9]+ passed, 0 failed")

  add_executable(dal_padtool tools/dal_padtool/main.cpp)
  target_compile_options(dal_padtool PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_padtool PRIVATE dal_input_gameinput)
endfunction()
