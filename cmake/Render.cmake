include_guard(GLOBAL)

# Vulkan-Headers, volk and VulkanMemoryAllocator come from pinned submodules, so the headers do not
# depend on the SDK installed on a machine. volk is built from its single C file with the project's
# default flags (no -Werror); SYSTEM keeps the third-party headers out of our warnings. Nothing is
# fetched or generated at configure time.
function(tox2_render_add_libraries)
  foreach(dir Vulkan-Headers volk VulkanMemoryAllocator)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.md AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.txt AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE)
      message(FATAL_ERROR
        "TOX2: externals/${dir} is missing or empty. Run: git submodule update --init")
    endif()
  endforeach()

  add_library(volk STATIC ${CMAKE_SOURCE_DIR}/externals/volk/volk.c)
  target_include_directories(volk SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/externals/volk
    ${CMAKE_SOURCE_DIR}/externals/Vulkan-Headers/include)
  # No link against the Vulkan loader import library: volk loads it at run time.
  target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
  target_compile_options(volk PRIVATE -march=x86-64-v3)
endfunction()

# The run-time shader compiler is the SDK's static shaderc, located through $VULKAN_SDK. The
# debug variant (shaderc_combinedd.lib) is built against the debug CRT (/MDd) and must be used
# with Debug only; every other configuration uses the release library (/MD).
function(tox2_render_add_shaderc)
  if(NOT DEFINED ENV{VULKAN_SDK} OR "$ENV{VULKAN_SDK}" STREQUAL "")
    message(FATAL_ERROR
      "TOX2: VULKAN_SDK is not set; expected $VULKAN_SDK/Lib/shaderc_combined.lib "
      "and $VULKAN_SDK/Lib/shaderc_combinedd.lib")
  endif()
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" sdk)
  set(release_lib "${sdk}/Lib/shaderc_combined.lib")
  set(debug_lib "${sdk}/Lib/shaderc_combinedd.lib")
  foreach(lib ${release_lib} ${debug_lib})
    if(NOT EXISTS "${lib}")
      message(FATAL_ERROR "TOX2: missing ${lib} (expected under $VULKAN_SDK/Lib)")
    endif()
  endforeach()
  if(NOT EXISTS "${sdk}/Include/shaderc/shaderc.hpp")
    message(FATAL_ERROR "TOX2: missing $VULKAN_SDK/Include/shaderc/shaderc.hpp")
  endif()

  add_library(shaderc_combined STATIC IMPORTED GLOBAL)
  set_target_properties(shaderc_combined PROPERTIES
    IMPORTED_LOCATION "${release_lib}"
    IMPORTED_LOCATION_DEBUG "${debug_lib}")
  target_include_directories(shaderc_combined SYSTEM INTERFACE "${sdk}/Include")
  message(STATUS "TOX2: shaderc: shaderc_combined.lib / shaderc_combinedd.lib from $VULKAN_SDK/Lib")
endfunction()

# NVIDIA Streamline (DLSS) is optional. TOX2_ENABLE_DLSS: AUTO uses the SDK under TOX2_STREAMLINE_DIR when
# present, ON requires it, OFF compiles the stub. The SDK is never linked: sl.interposer.dll is loaded at run
# time (its import library exports the same Vulkan names as volk's global function pointers). Sets
# TOX2_STREAMLINE_ENABLED and TOX2_STREAMLINE_RESOLVED_DIR in the caller's scope.
function(tox2_render_find_streamline)
  include("${CMAKE_SOURCE_DIR}/cmake/streamline.cmake")
  set(TOX2_ENABLE_DLSS "AUTO" CACHE STRING "Use the NVIDIA Streamline SDK for DLAA (AUTO, ON or OFF)")
  set_property(CACHE TOX2_ENABLE_DLSS PROPERTY STRINGS AUTO ON OFF)
  set(TOX2_STREAMLINE_DIR "${CMAKE_SOURCE_DIR}/externals/Streamline" CACHE PATH
    "Root of the unpacked NVIDIA Streamline SDK")

  string(TOUPPER "${TOX2_ENABLE_DLSS}" mode)
  if(NOT mode MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "TOX2: TOX2_ENABLE_DLSS must be AUTO, ON or OFF (got '${TOX2_ENABLE_DLSS}')")
  endif()

  set(enabled FALSE)
  if(mode STREQUAL "OFF")
    message(STATUS "TOX2: Streamline: off (TOX2_ENABLE_DLSS=OFF)")
  else()
    set(sdk "${TOX2_STREAMLINE_DIR}")
    set(found TRUE)
    foreach(file include/sl.h bin/x64/sl.interposer.dll bin/x64/sl.common.dll bin/x64/sl.dlss.dll
                 bin/x64/nvngx_dlss.dll)
      if(NOT EXISTS "${sdk}/${file}")
        set(found FALSE)
      endif()
    endforeach()
    if(found)
      set(enabled TRUE)
      set(marker "${sdk}/.verified-sha256")
      set(note ", not verified by tools/fetch-streamline.ps1")
      if(EXISTS "${marker}")
        file(READ "${marker}" marker_hash)
        string(STRIP "${marker_hash}" marker_hash)
        if(marker_hash STREQUAL "${TOX2_STREAMLINE_SHA256}")
          set(note "")
        endif()
        # Re-fetching another pin must re-run the configure step. A missing file would make Ninja re-run it on
        # every build, so only an existing marker is a dependency.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
      endif()
      message(STATUS "TOX2: Streamline: ${sdk} (v${TOX2_STREAMLINE_VERSION})${note}")
    else()
      set(hint "${sdk}/include/sl.h); DLAA disabled. Run: pwsh -NoProfile -File tools/fetch-streamline.ps1")
      if(mode STREQUAL "ON")
        message(FATAL_ERROR "TOX2: Streamline: not found (${hint}")
      endif()
      message(STATUS "TOX2: Streamline: not found (${hint}")
    endif()
  endif()

  set(TOX2_STREAMLINE_ENABLED ${enabled} PARENT_SCOPE)
  set(TOX2_STREAMLINE_RESOLVED_DIR "${TOX2_STREAMLINE_DIR}" PARENT_SCOPE)
endfunction()

# Copies the Streamline runtime next to the executables, with the licence texts the redistribution terms ask
# for. The DLLs are needed from the first use of the loader on, so every test target depends on the copy.
function(tox2_render_add_streamline_runtime)
  set(sdk "${TOX2_STREAMLINE_RESOLVED_DIR}")
  set(bin_dir "${CMAKE_BINARY_DIR}/bin")
  set(licence_dir "${bin_dir}/licenses/streamline")
  set(copied "")
  # NvLowLatencyVk.dll (Reflex) is left out: its licence does not list it as distributable. On Vulkan Streamline
  # uses VK_NV_low_latency2 instead; sl.common's error about the missing DLL is logged at debug level.
  foreach(name sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
    add_custom_command(OUTPUT "${bin_dir}/${name}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/bin/x64/${name}" "${bin_dir}/${name}"
      DEPENDS "${sdk}/bin/x64/${name}"
      VERBATIM)
    list(APPEND copied "${bin_dir}/${name}")
  endforeach()
  # Frame generation is optional: a missing DLL only makes it unavailable at run time, DLAA is unaffected.
  set(missing_frame_gen "")
  foreach(name sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
    if(NOT EXISTS "${sdk}/bin/x64/${name}")
      list(APPEND missing_frame_gen "${name}")
      continue()
    endif()
    add_custom_command(OUTPUT "${bin_dir}/${name}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/bin/x64/${name}" "${bin_dir}/${name}"
      DEPENDS "${sdk}/bin/x64/${name}"
      VERBATIM)
    list(APPEND copied "${bin_dir}/${name}")
  endforeach()
  if(missing_frame_gen)
    list(JOIN missing_frame_gen ", " missing_text)
    message(STATUS "TOX2: Streamline: frame generation DLLs missing (${missing_text})")
  endif()
  foreach(pair "license.txt|LICENSE.txt" "bin/x64/nvngx_dlss.license.txt|nvngx_dlss.license.txt"
               "3rd-party-licenses.md|3rd-party-licenses.md")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 from)
    list(GET pair 1 to)
    add_custom_command(OUTPUT "${licence_dir}/${to}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/${from}" "${licence_dir}/${to}"
      DEPENDS "${sdk}/${from}"
      VERBATIM)
    list(APPEND copied "${licence_dir}/${to}")
  endforeach()
  add_custom_target(tox2_streamline_runtime ALL DEPENDS ${copied})
endfunction()

# The renderer library. tox2_runtime_core stays GPU-free: only targets that link tox2_render see
# Vulkan. Call after tox2_runtime_add_core() and enable_testing().
function(tox2_render_add)
  tox2_render_add_libraries()
  tox2_render_add_shaderc()
  tox2_render_find_streamline()

  add_library(tox2_render STATIC
    runtime/src/render/dlaa_pass.cpp
    runtime/src/render/fg_motion.cpp
    runtime/src/render/fp_translator.cpp
    runtime/src/render/frame_dump.cpp
    runtime/src/render/frame_gen_runtime.cpp
    runtime/src/render/image_pool.cpp
    runtime/src/render/shader_compiler.cpp
    runtime/src/render/vertex_convert.cpp
    runtime/src/render/vk_context.cpp
    runtime/src/render/vk_pipelines.cpp
    runtime/src/render/vk_renderer.cpp
    runtime/src/render/vk_textures.cpp
    runtime/src/render/vk_window.cpp
    runtime/src/render/vp_translator.cpp)
  target_compile_options(tox2_render PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(tox2_render PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  target_include_directories(tox2_render SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/externals/VulkanMemoryAllocator/include)
  # The allocator resolves its Vulkan entry points through volk's vkGet*ProcAddr; it must see the
  # same settings in every translation unit that includes its header.
  target_compile_definitions(tox2_render PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1003000)
  target_link_libraries(tox2_render PUBLIC volk tox2_runtime_core)
  target_link_libraries(tox2_render PRIVATE shaderc_combined)
  # ImmAssociateContext turns the IME off for the game window.
  target_link_libraries(tox2_render PRIVATE imm32)
  # PRIVATE: libpng types stay inside frame_dump.cpp.
  target_link_libraries(tox2_render PRIVATE png_static)
  if(TOX2_STREAMLINE_ENABLED)
    target_sources(tox2_render PRIVATE runtime/src/render/streamline.cpp)
    target_include_directories(tox2_render SYSTEM PRIVATE "${TOX2_STREAMLINE_RESOLVED_DIR}/include")
    target_compile_definitions(tox2_render PUBLIC TOX2_HAS_STREAMLINE=1)
  else()
    target_sources(tox2_render PRIVATE runtime/src/render/streamline_off.cpp)
    target_compile_definitions(tox2_render PUBLIC TOX2_HAS_STREAMLINE=0)
  endif()

  add_executable(tox2_render_tests runtime/tests/render_test.cpp)
  target_compile_options(tox2_render_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(tox2_render_tests PRIVATE tox2_render)

  add_test(NAME render_core COMMAND tox2_render_tests)
  set_tests_properties(render_core PROPERTIES
    PASS_REGULAR_EXPRESSION
      "render tests: [0-9]+ passed, 0 failed|render tests: skipped: no Vulkan device")

  # Needs no GPU. The dumped programs come from generated/rsx; the test skips them when absent.
  add_executable(tox2_vp_translator_tests runtime/tests/vp_translator_test.cpp)
  target_compile_options(tox2_vp_translator_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_compile_definitions(tox2_vp_translator_tests PRIVATE
    TOX2_RSX_DUMP_DIR="${CMAKE_SOURCE_DIR}/generated/rsx")
  target_link_libraries(tox2_vp_translator_tests PRIVATE tox2_render)
  add_test(NAME vp_translator COMMAND tox2_vp_translator_tests)
  set_tests_properties(vp_translator PROPERTIES
    PASS_REGULAR_EXPRESSION "vp translator tests: [0-9]+ passed, 0 failed")

  add_executable(tox2_fp_translator_tests runtime/tests/fp_translator_test.cpp)
  target_compile_options(tox2_fp_translator_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_compile_definitions(tox2_fp_translator_tests PRIVATE
    TOX2_RSX_DUMP_DIR="${CMAKE_SOURCE_DIR}/generated/rsx")
  target_link_libraries(tox2_fp_translator_tests PRIVATE tox2_render)
  add_test(NAME fp_translator COMMAND tox2_fp_translator_tests)
  set_tests_properties(fp_translator PROPERTIES
    PASS_REGULAR_EXPRESSION "fp translator tests: [0-9]+ passed, 0 failed")

  # Header-only maths; needs no GPU.
  add_executable(tox2_frame_gen_tests runtime/tests/frame_gen_test.cpp)
  target_compile_options(tox2_frame_gen_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(tox2_frame_gen_tests PRIVATE tox2_render)
  add_test(NAME frame_gen COMMAND tox2_frame_gen_tests)
  set_tests_properties(frame_gen PROPERTIES
    PASS_REGULAR_EXPRESSION "frame gen tests: [0-9]+ passed, 0 failed")

  # Replays a capture without the game: prints the command list from the frame builder, renders it
  # headless and writes the displayed frame as a PNG.
  add_executable(tox2_rsxreplay tools/tox2_rsxreplay/main.cpp)
  target_compile_options(tox2_rsxreplay PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(tox2_rsxreplay PRIVATE tox2_render)

  if(TOX2_STREAMLINE_ENABLED)
    tox2_render_add_streamline_runtime()
    add_dependencies(tox2_render_tests tox2_streamline_runtime)
    add_dependencies(tox2_rsxreplay tox2_streamline_runtime)
    # The game loads the DLLs from its own directory. It is defined earlier, and only when the generated tree exists.
    if(TARGET tox2_game)
      add_dependencies(tox2_game tox2_streamline_runtime)
    endif()
  endif()

  include("${CMAKE_SOURCE_DIR}/cmake/rsx_replay.cmake")
  set(replay_capture
    "${CMAKE_SOURCE_DIR}/generated/rsx/captures/${TOX2_RSX_REPLAY_CAPTURE}.tox2cap")
  set(replay_reference
    "${CMAKE_SOURCE_DIR}/generated/rsx/reference/${TOX2_RSX_REPLAY_CAPTURE}.png")
  set(replay_args "${replay_capture}" --out "${CMAKE_BINARY_DIR}/replay")
  set(replay_register TRUE)
  if(NOT "${TOX2_RSX_REPLAY_REFERENCE_SHA256}" STREQUAL "")
    # A pinned reference must exist and match, so the thresholds are never applied to another image.
    set(replay_register FALSE)
    # The pin is checked at configure time, so a changed or newly added reference must re-run it.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${replay_reference}")
    if(NOT EXISTS "${replay_reference}")
      message(STATUS "TOX2: rsx_replay test not registered "
        "(generated/rsx/reference/${TOX2_RSX_REPLAY_CAPTURE}.png not found)")
    else()
      file(SHA256 "${replay_reference}" replay_reference_sha256)
      if(NOT replay_reference_sha256 STREQUAL "${TOX2_RSX_REPLAY_REFERENCE_SHA256}")
        message(STATUS "TOX2: rsx_replay test not registered "
          "(generated/rsx/reference/${TOX2_RSX_REPLAY_CAPTURE}.png does not match the pinned SHA-256)")
      else()
        set(replay_register TRUE)
        list(APPEND replay_args --compare "${replay_reference}")
        if(NOT "${TOX2_RSX_REPLAY_MAX_DIFF}" STREQUAL "")
          list(APPEND replay_args --max-diff "${TOX2_RSX_REPLAY_MAX_DIFF}")
        endif()
        if(NOT "${TOX2_RSX_REPLAY_MEAN_DIFF}" STREQUAL "")
          list(APPEND replay_args --max-mean "${TOX2_RSX_REPLAY_MEAN_DIFF}")
        endif()
        if(NOT "${TOX2_RSX_REPLAY_DIFFERING_PERCENT}" STREQUAL "")
          list(APPEND replay_args --max-differing "${TOX2_RSX_REPLAY_DIFFERING_PERCENT}")
        endif()
      endif()
    endif()
  endif()
  if(NOT EXISTS "${replay_capture}")
    set(replay_register FALSE)
  endif()
  if(replay_register)
    add_test(NAME rsx_replay COMMAND tox2_rsxreplay ${replay_args})
    set_tests_properties(rsx_replay PROPERTIES
      PASS_REGULAR_EXPRESSION "replay: [0-9]+ commands, [0-9]+ draws, [0-9]+ skipped"
      FAIL_REGULAR_EXPRESSION "validation: [a-z]+, [1-9][0-9]* errors;error: ")
  elseif(NOT EXISTS "${replay_capture}")
    message(STATUS "TOX2: rsx_replay test not registered "
      "(generated/rsx/captures/${TOX2_RSX_REPLAY_CAPTURE}.tox2cap not found)")
  endif()

  # Smoke test of the --repeat measurement the fixed-input comparisons use.
  set(replay_repeat_capture
    "${CMAKE_SOURCE_DIR}/generated/rsx/captures/${TOX2_RSX_REPLAY_REPEAT_CAPTURE}.tox2cap")
  if(EXISTS "${replay_repeat_capture}")
    add_test(NAME rsx_replay_repeat
      COMMAND tox2_rsxreplay "${replay_repeat_capture}" --repeat 3 --quiet --no-validation)
    set_tests_properties(rsx_replay_repeat PROPERTIES
      PASS_REGULAR_EXPRESSION
      "cycles: ${TOX2_RSX_REPLAY_REPEAT_CAPTURE}, 3 iterations \\(first excluded\\), median thread cycles per iteration: build [0-9]+, record [0-9]+, total [0-9]+"
      FAIL_REGULAR_EXPRESSION "error: ")

    # The builder and render lines carry the texture unit cache and texture bind counters.
    add_test(NAME rsx_replay_builder_counters
      COMMAND tox2_rsxreplay "${replay_repeat_capture}" --repeat 3 --quiet --no-validation)
    set_tests_properties(rsx_replay_builder_counters PROPERTIES
      PASS_REGULAR_EXPRESSION
      "builder: per iteration .*, texture unit cache [0-9]+ hits [0-9]+ misses"
      FAIL_REGULAR_EXPRESSION "error: ")
    add_test(NAME rsx_replay_hash_counters
      COMMAND tox2_rsxreplay "${replay_repeat_capture}" --repeat 3 --quiet --no-validation)
    set_tests_properties(rsx_replay_hash_counters PROPERTIES
      PASS_REGULAR_EXPRESSION
      "builder: per iteration .*, texture hash skips [0-9]+ \\(safety hashes [0-9]+, unreported changes [0-9]+\\)"
      FAIL_REGULAR_EXPRESSION "error: ")
    add_test(NAME rsx_replay_render_counters
      COMMAND tox2_rsxreplay "${replay_repeat_capture}" --repeat 3 --quiet --no-validation)
    set_tests_properties(rsx_replay_render_counters PROPERTIES
      PASS_REGULAR_EXPRESSION
      "render: per iteration .*, texture binds [0-9]+ cached [0-9]+ full;"
      FAIL_REGULAR_EXPRESSION "error: ")
  else()
    message(STATUS "TOX2: rsx_replay_repeat test not registered "
      "(generated/rsx/captures/${TOX2_RSX_REPLAY_REPEAT_CAPTURE}.tox2cap not found)")
  endif()
endfunction()
