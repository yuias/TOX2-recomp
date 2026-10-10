include_guard(GLOBAL)

# spdlog is built as a static library rather than header-only: its sources then compile with the
# project's default flags (no -Werror) and keep <windows.h> out of runtime translation units.
# SPDLOG_USE_STD_FORMAT reuses the std::format the runtime already depends on instead of bundled fmt.
function(tox2_runtime_add_spdlog)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/spdlog/CMakeLists.txt)
    message(FATAL_ERROR
      "TOX2: externals/spdlog is missing or empty. Run: git submodule update --init")
  endif()
  set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
  set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/externals/spdlog
    ${CMAKE_BINARY_DIR}/externals/spdlog EXCLUDE_FROM_ALL)
endfunction()

# zlib and libpng are built from the submodules as static libraries, so their sources compile
# with the project's default flags (no -Werror). libpng's CMake finds zlib through FindZLIB, which
# only sees installed copies; the cache variables below point it at the zlib target built here.
# Nothing is installed and nothing is fetched or generated at configure time: pnglibconf.h comes
# from libpng's prebuilt copy instead of the awk-driven generator.
function(tox2_runtime_add_libpng)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/zlib/CMakeLists.txt OR
     NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/libpng/CMakeLists.txt)
    message(FATAL_ERROR
      "TOX2: externals/zlib or externals/libpng is missing or empty. Run: git submodule update --init")
  endif()

  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/externals/zlib
    ${CMAKE_BINARY_DIR}/externals/zlib EXCLUDE_FROM_ALL)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  set(ZLIB_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/externals/zlib CACHE PATH "" FORCE)
  set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)

  set(PNG_SHARED OFF CACHE BOOL "" FORCE)
  set(PNG_STATIC ON CACHE BOOL "" FORCE)
  set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
  set(PNG_TESTS OFF CACHE BOOL "" FORCE)
  set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
  set(PNG_LIBCONF_HEADER
    ${CMAKE_SOURCE_DIR}/externals/libpng/scripts/pnglibconf.h.prebuilt CACHE FILEPATH "" FORCE)
  set(ld-version-script OFF CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/externals/libpng
    ${CMAKE_BINARY_DIR}/externals/libpng EXCLUDE_FROM_ALL)
endfunction()

# yyjson is built as a static library from its own CMake, so its C sources compile with the
# project's default flags (no -Werror). Tests, docs and install rules stay off. SYSTEM keeps its
# header out of the warnings of the translation units that include it.
function(tox2_runtime_add_yyjson)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/yyjson/CMakeLists.txt)
    message(FATAL_ERROR
      "TOX2: externals/yyjson is missing or empty. Run: git submodule update --init")
  endif()
  set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YYJSON_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
  set(YYJSON_BUILD_MISC OFF CACHE BOOL "" FORCE)
  set(YYJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
  set(YYJSON_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/externals/yyjson
    ${CMAKE_BINARY_DIR}/externals/yyjson EXCLUDE_FROM_ALL)
  set_target_properties(yyjson PROPERTIES SYSTEM ON)
endfunction()

# Runtime core: everything that does not reference generated symbols, so it builds and its tests
# run without a generated tree. Call after enable_testing().
function(tox2_runtime_add_core)
  tox2_runtime_add_spdlog()
  tox2_runtime_add_libpng()
  tox2_runtime_add_yyjson()

  add_library(tox2_runtime_core STATIC
    runtime/src/audio/audio_engine.cpp
    runtime/src/audio/audio_output.cpp
    runtime/src/audio/wav_file.cpp
    runtime/src/core/boot.cpp
    runtime/src/core/census.cpp
    runtime/src/core/cpu_hooks.cpp
    runtime/src/core/dispatch.cpp
    runtime/src/core/elf_loader.cpp
    runtime/src/core/fatal.cpp
    runtime/src/core/guest_memory.cpp
    runtime/src/core/log.cpp
    runtime/src/core/param_sfo.cpp
    runtime/src/core/reservation.cpp
    runtime/src/core/threads.cpp
    runtime/src/core/timer_resolution.cpp
    runtime/src/hle/cell_atrac.cpp
    runtime/src/hle/cell_atrac_stream.cpp
    runtime/src/hle/cell_audio.cpp
    runtime/src/hle/cell_audio_ports.cpp
    runtime/src/hle/cell_fs.cpp
    runtime/src/hle/cell_game.cpp
    runtime/src/hle/cell_gcm_sys.cpp
    runtime/src/hle/cell_msg_dialog.cpp
    runtime/src/hle/cell_pad.cpp
    runtime/src/hle/cell_pamf.cpp
    runtime/src/hle/cell_png_dec.cpp
    runtime/src/hle/cell_sail_adapters.cpp
    runtime/src/hle/cell_sail_player.cpp
    runtime/src/hle/cell_save_data.cpp
    runtime/src/hle/cell_sysmodule.cpp
    runtime/src/hle/cell_sysutil.cpp
    runtime/src/hle/guest_printf.cpp
    runtime/src/hle/hle.cpp
    runtime/src/hle/libsre.cpp
    runtime/src/hle/sail_service.cpp
    runtime/src/hle/sce_np.cpp
    runtime/src/hle/sce_np_trophy.cpp
    runtime/src/hle/sys_prx_for_user.cpp
    runtime/src/hle/sys_prx_for_user_libc.cpp
    runtime/src/input/gamepad_hub.cpp
    runtime/src/input/gamepad_map.cpp
    runtime/src/input/gamepad_probe.cpp
    runtime/src/input/gamepad_select.cpp
    runtime/src/input/keyboard_pad.cpp
    runtime/src/input/pad_buttons.cpp
    runtime/src/input/pad_script.cpp
    runtime/src/lv2/sys_event.cpp
    runtime/src/lv2/sys_fs.cpp
    runtime/src/lv2/sys_misc.cpp
    runtime/src/lv2/sys_mutex_cond.cpp
    runtime/src/lv2/sys_ppu_thread.cpp
    runtime/src/lv2/sys_semaphore.cpp
    runtime/src/lv2/sys_spu.cpp
    runtime/src/lv2/syscalls.cpp
    runtime/src/lv2/vfs.cpp
    runtime/src/lv2/vfs_config.cpp
    runtime/src/media/at3_file.cpp
    runtime/src/media/movie_player.cpp
    runtime/src/media/pamf.cpp
    runtime/src/rsx/fifo_parser.cpp
    runtime/src/rsx/gcm_interrupts.cpp
    runtime/src/rsx/rsx_capture.cpp
    runtime/src/rsx/rsx_census.cpp
    runtime/src/rsx/rsx_decode.cpp
    runtime/src/rsx/rsx_frame_builder.cpp
    runtime/src/rsx/rsx_registers.cpp
    runtime/src/rsx/rsx_state.cpp
    runtime/src/rsx/rsx_thread.cpp
    runtime/src/rsx/rsx_unimplemented.cpp
    runtime/src/spu/spu_dispatch.cpp
    runtime/src/spu/spu_mfc.cpp
    runtime/src/spu/spu_thread.cpp)
  target_compile_options(tox2_runtime_core PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(tox2_runtime_core PUBLIC
    ${CMAKE_SOURCE_DIR}/runtime/src
    ${CMAKE_SOURCE_DIR}/runtime/include)
  target_link_libraries(tox2_runtime_core PUBLIC bcrypt)
  # timeBeginPeriod (core/timer_resolution.cpp).
  target_link_libraries(tox2_runtime_core PRIVATE winmm)
  # PRIVATE: spdlog types stay out of core/log.h, so consumers never compile against its headers.
  target_link_libraries(tox2_runtime_core PRIVATE spdlog::spdlog)
  # PRIVATE as well: the PNG codec stays inside cell_png_dec.cpp.
  target_link_libraries(tox2_runtime_core PRIVATE png_static)
  # PRIVATE as well: JSON types stay inside vfs_config.cpp and rsx_census.cpp.
  target_link_libraries(tox2_runtime_core PRIVATE yyjson)

  add_executable(tox2_runtime_tests runtime/tests/runtime_core_test.cpp)
  target_compile_options(tox2_runtime_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  # The tests build their PNG inputs with libpng's writer.
  target_link_libraries(tox2_runtime_tests PRIVATE tox2_runtime_core png_static)

  add_test(NAME runtime_core COMMAND tox2_runtime_tests)
  set_tests_properties(runtime_core PROPERTIES
    PASS_REGULAR_EXPRESSION "runtime core tests: [0-9]+ passed, 0 failed")

  # The movie pipeline's core needs no FFmpeg, so its tests link the core library only.
  add_executable(tox2_media_core_tests runtime/tests/media_core_test.cpp)
  target_compile_options(tox2_media_core_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(tox2_media_core_tests PRIVATE tox2_runtime_core)

  add_test(NAME media_core COMMAND tox2_media_core_tests)
  set_tests_properties(media_core PROPERTIES
    PASS_REGULAR_EXPRESSION "media core tests: [0-9]+ passed, 0 failed")

  add_executable(tox2_spu_runtime_tests runtime/tests/spu_runtime_test.cpp)
  target_compile_options(tox2_spu_runtime_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(tox2_spu_runtime_tests PRIVATE tox2_runtime_core)

  add_test(NAME spu_runtime COMMAND tox2_spu_runtime_tests)
  set_tests_properties(spu_runtime PROPERTIES
    PASS_REGULAR_EXPRESSION "spu runtime tests: [0-9]+ passed, 0 failed")
endfunction()
