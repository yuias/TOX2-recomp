include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/gameinput.cmake")

# The Microsoft.GameInput package is fetched and verified by tools/fetch-gameinput.ps1 into
# generated/deps/gameinput. Needs a successful fetch, which leaves a marker holding the SHA-256 of
# the pinned .nupkg.
#
# Sets in the caller's scope:
#   TOX2_GAMEINPUT_INCLUDE_DIR  the v3 header directory. Targets must add it as a SYSTEM include:
#                              the Windows SDK ships an older GameInput.h on the default include path.
#   TOX2_GAMEINPUT_LIB          GameInput.lib, a static loader that resolves the runtime with
#                              LoadLibrary (it imports nothing from a GameInput DLL). Link it by this
#                              full path: the SDK's own gameinput.lib is an import library.
#
# The runtime is not part of the build: the user installs redist/GameInputRedist.msi, which the
# README points to (checked here so a broken extraction is caught). A copy of its DLL next to the
# executable would take precedence over the installed one and hide Xbox pads.
function(tox2_input_find_gameinput)
  set(TOX2_GAMEINPUT_DIR "" CACHE PATH
    "Extracted Microsoft.GameInput package (default: generated/deps/gameinput/<pinned version>)")
  if("${TOX2_GAMEINPUT_DIR}" STREQUAL "")
    set(gameinput_dir "${CMAKE_SOURCE_DIR}/generated/deps/gameinput/${TOX2_GAMEINPUT_VERSION}")
  else()
    set(gameinput_dir "${TOX2_GAMEINPUT_DIR}")
  endif()

  set(marker "${gameinput_dir}/.verified-sha256")
  # A fetch after this configure must re-run it.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
  set(fetch_hint "Run: pwsh -NoProfile -File tools/fetch-gameinput.ps1")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "TOX2: GameInput is not fetched (${marker} not found). ${fetch_hint}")
  endif()
  file(READ "${marker}" marker_hash)
  string(STRIP "${marker_hash}" marker_hash)
  string(TOLOWER "${marker_hash}" marker_hash)
  string(TOLOWER "${TOX2_GAMEINPUT_SHA256}" pinned_hash)
  if(NOT marker_hash STREQUAL pinned_hash)
    message(FATAL_ERROR
      "TOX2: GameInput in ${gameinput_dir} does not match the pin in cmake/gameinput.cmake. ${fetch_hint}")
  endif()
  foreach(path native/include/GameInput.h native/lib/x64/GameInput.lib redist/GameInputRedist.msi)
    if(NOT EXISTS "${gameinput_dir}/${path}")
      message(FATAL_ERROR "TOX2: ${gameinput_dir}/${path} is missing. ${fetch_hint} -Force")
    endif()
  endforeach()

  set(TOX2_GAMEINPUT_INCLUDE_DIR "${gameinput_dir}/native/include" PARENT_SCOPE)
  set(TOX2_GAMEINPUT_LIB "${gameinput_dir}/native/lib/x64/GameInput.lib" PARENT_SCOPE)

  # Relative to the source tree so the log carries no machine-specific prefix.
  file(RELATIVE_PATH gameinput_rel "${CMAKE_SOURCE_DIR}" "${gameinput_dir}")
  message(STATUS "TOX2: GameInput: ${gameinput_rel}")
endfunction()

# The input libraries and tools. Call after tox2_runtime_add_core() and enable_testing().
function(tox2_input_add)
  if(WIN32)
    tox2_input_find_gameinput()

    # The only target that sees GameInput.h and Windows.h for gamepads; tox2_runtime_core stays free
    # of them. The v3 header directory is a SYSTEM include so it is searched before the Windows SDK's
    # older GameInput.h. GameInput.lib is the package's static loader (it resolves the installed
    # runtime with LoadLibrary), so no GameInput DLL appears in an import table.
    add_library(tox2_input_gameinput STATIC runtime/src/input/gameinput_backend.cpp)
    target_compile_options(tox2_input_gameinput PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_include_directories(tox2_input_gameinput SYSTEM PRIVATE "${TOX2_GAMEINPUT_INCLUDE_DIR}")
    target_link_libraries(tox2_input_gameinput PUBLIC tox2_runtime_core PRIVATE "${TOX2_GAMEINPUT_LIB}")
  endif()

  # Pure core code behind the GamepadBackend interface; no GameInput or Windows.
  add_executable(tox2_input_core_tests runtime/tests/input_core_test.cpp)
  target_compile_options(tox2_input_core_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_input_core_tests PRIVATE tox2_runtime_core)

  add_test(NAME input_core COMMAND tox2_input_core_tests)
  set_tests_properties(input_core PROPERTIES
    PASS_REGULAR_EXPRESSION "input core tests: [0-9]+ passed, 0 failed")

  if(WIN32)
    # Exits 77 (skipped) when no GameInput runtime with API version 3 is available.
    add_executable(tox2_input_gameinput_tests runtime/tests/input_gameinput_test.cpp)
    target_compile_options(tox2_input_gameinput_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_input_gameinput_tests PRIVATE tox2_input_gameinput)

    add_test(NAME input_gameinput COMMAND tox2_input_gameinput_tests)
    set_tests_properties(input_gameinput PROPERTIES
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "input gameinput tests: [0-9]+ passed, 0 failed")

    add_executable(tox2_padtool tools/tox2_padtool/main.cpp)
    target_compile_options(tox2_padtool PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_padtool PRIVATE tox2_input_gameinput)
  endif()
endfunction()
