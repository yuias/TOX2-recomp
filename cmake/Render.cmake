include_guard(GLOBAL)

# Vulkan-Headers, volk and VulkanMemoryAllocator come from pinned submodules, so the headers do not
# depend on the SDK installed on a machine. volk is built from its single C file with the project's
# default flags (no -Werror); SYSTEM keeps the third-party headers out of our warnings. Nothing is
# fetched or generated at configure time.
function(dal_render_add_libraries)
  foreach(dir Vulkan-Headers volk VulkanMemoryAllocator)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.md AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.txt AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE)
      message(FATAL_ERROR
        "DAL: externals/${dir} is missing or empty. Run: git submodule update --init")
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
function(dal_render_add_shaderc)
  if(NOT DEFINED ENV{VULKAN_SDK} OR "$ENV{VULKAN_SDK}" STREQUAL "")
    message(FATAL_ERROR
      "DAL: VULKAN_SDK is not set; expected $VULKAN_SDK/Lib/shaderc_combined.lib "
      "and $VULKAN_SDK/Lib/shaderc_combinedd.lib")
  endif()
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" sdk)
  set(release_lib "${sdk}/Lib/shaderc_combined.lib")
  set(debug_lib "${sdk}/Lib/shaderc_combinedd.lib")
  foreach(lib ${release_lib} ${debug_lib})
    if(NOT EXISTS "${lib}")
      message(FATAL_ERROR "DAL: missing ${lib} (expected under $VULKAN_SDK/Lib)")
    endif()
  endforeach()
  if(NOT EXISTS "${sdk}/Include/shaderc/shaderc.hpp")
    message(FATAL_ERROR "DAL: missing $VULKAN_SDK/Include/shaderc/shaderc.hpp")
  endif()

  add_library(shaderc_combined STATIC IMPORTED GLOBAL)
  set_target_properties(shaderc_combined PROPERTIES
    IMPORTED_LOCATION "${release_lib}"
    IMPORTED_LOCATION_DEBUG "${debug_lib}")
  target_include_directories(shaderc_combined SYSTEM INTERFACE "${sdk}/Include")
  message(STATUS "DAL: shaderc: shaderc_combined.lib / shaderc_combinedd.lib from $VULKAN_SDK/Lib")
endfunction()

# The renderer library. dal_runtime_core stays GPU-free: only targets that link dal_render see
# Vulkan. Call after dal_runtime_add_core() and enable_testing().
function(dal_render_add)
  dal_render_add_libraries()
  dal_render_add_shaderc()

  add_library(dal_render STATIC
    runtime/src/render/fp_translator.cpp
    runtime/src/render/frame_dump.cpp
    runtime/src/render/shader_compiler.cpp
    runtime/src/render/vertex_convert.cpp
    runtime/src/render/vk_context.cpp
    runtime/src/render/vk_pipelines.cpp
    runtime/src/render/vk_renderer.cpp
    runtime/src/render/vk_textures.cpp
    runtime/src/render/vk_window.cpp
    runtime/src/render/vp_translator.cpp)
  target_compile_options(dal_render PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_render PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  target_include_directories(dal_render SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/externals/VulkanMemoryAllocator/include)
  # The allocator resolves its Vulkan entry points through volk's vkGet*ProcAddr; it must see the
  # same settings in every translation unit that includes its header.
  target_compile_definitions(dal_render PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1003000)
  target_link_libraries(dal_render PUBLIC volk dal_runtime_core)
  target_link_libraries(dal_render PRIVATE shaderc_combined)
  # ImmAssociateContext turns the IME off for the game window.
  target_link_libraries(dal_render PRIVATE imm32)
  # PRIVATE: libpng types stay inside frame_dump.cpp.
  target_link_libraries(dal_render PRIVATE png_static)

  add_executable(dal_render_tests runtime/tests/render_test.cpp)
  target_compile_options(dal_render_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_render_tests PRIVATE dal_render)

  add_test(NAME render_core COMMAND dal_render_tests)
  set_tests_properties(render_core PROPERTIES
    PASS_REGULAR_EXPRESSION
      "render tests: [0-9]+ passed, 0 failed|render tests: skipped: no Vulkan device")

  # Needs no GPU. The dumped programs come from generated/rsx; the test skips them when absent.
  add_executable(dal_vp_translator_tests runtime/tests/vp_translator_test.cpp)
  target_compile_options(dal_vp_translator_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_compile_definitions(dal_vp_translator_tests PRIVATE
    DAL_RSX_DUMP_DIR="${CMAKE_SOURCE_DIR}/generated/rsx")
  target_link_libraries(dal_vp_translator_tests PRIVATE dal_render)
  add_test(NAME vp_translator COMMAND dal_vp_translator_tests)
  set_tests_properties(vp_translator PROPERTIES
    PASS_REGULAR_EXPRESSION "vp translator tests: [0-9]+ passed, 0 failed")

  add_executable(dal_fp_translator_tests runtime/tests/fp_translator_test.cpp)
  target_compile_options(dal_fp_translator_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_compile_definitions(dal_fp_translator_tests PRIVATE
    DAL_RSX_DUMP_DIR="${CMAKE_SOURCE_DIR}/generated/rsx")
  target_link_libraries(dal_fp_translator_tests PRIVATE dal_render)
  add_test(NAME fp_translator COMMAND dal_fp_translator_tests)
  set_tests_properties(fp_translator PROPERTIES
    PASS_REGULAR_EXPRESSION "fp translator tests: [0-9]+ passed, 0 failed")

  # Replays a capture without the game: prints the command list from the frame builder, renders it
  # headless and writes the displayed frame as a PNG.
  add_executable(dal_rsxreplay tools/dal_rsxreplay/main.cpp)
  target_compile_options(dal_rsxreplay PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_rsxreplay PRIVATE dal_render)

  include("${CMAKE_SOURCE_DIR}/cmake/rsx_replay.cmake")
  set(replay_capture
    "${CMAKE_SOURCE_DIR}/generated/rsx/captures/${DAL_RSX_REPLAY_CAPTURE}.dalcap")
  set(replay_reference
    "${CMAKE_SOURCE_DIR}/generated/rsx/reference/${DAL_RSX_REPLAY_CAPTURE}.png")
  set(replay_args "${replay_capture}" --out "${CMAKE_BINARY_DIR}/replay")
  set(replay_register TRUE)
  if(NOT "${DAL_RSX_REPLAY_REFERENCE_SHA256}" STREQUAL "")
    # A pinned reference must exist and match, so the thresholds are never applied to another image.
    set(replay_register FALSE)
    # The pin is checked at configure time, so a changed or newly added reference must re-run it.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${replay_reference}")
    if(NOT EXISTS "${replay_reference}")
      message(STATUS "DAL: rsx_replay test not registered "
        "(generated/rsx/reference/${DAL_RSX_REPLAY_CAPTURE}.png not found)")
    else()
      file(SHA256 "${replay_reference}" replay_reference_sha256)
      if(NOT replay_reference_sha256 STREQUAL "${DAL_RSX_REPLAY_REFERENCE_SHA256}")
        message(STATUS "DAL: rsx_replay test not registered "
          "(generated/rsx/reference/${DAL_RSX_REPLAY_CAPTURE}.png does not match the pinned SHA-256)")
      else()
        set(replay_register TRUE)
        list(APPEND replay_args --compare "${replay_reference}")
        if(NOT "${DAL_RSX_REPLAY_MAX_DIFF}" STREQUAL "")
          list(APPEND replay_args --max-diff "${DAL_RSX_REPLAY_MAX_DIFF}")
        endif()
        if(NOT "${DAL_RSX_REPLAY_MEAN_DIFF}" STREQUAL "")
          list(APPEND replay_args --max-mean "${DAL_RSX_REPLAY_MEAN_DIFF}")
        endif()
        if(NOT "${DAL_RSX_REPLAY_DIFFERING_PERCENT}" STREQUAL "")
          list(APPEND replay_args --max-differing "${DAL_RSX_REPLAY_DIFFERING_PERCENT}")
        endif()
      endif()
    endif()
  endif()
  if(NOT EXISTS "${replay_capture}")
    set(replay_register FALSE)
  endif()
  if(replay_register)
    add_test(NAME rsx_replay COMMAND dal_rsxreplay ${replay_args})
    set_tests_properties(rsx_replay PROPERTIES
      PASS_REGULAR_EXPRESSION "replay: [0-9]+ commands, [0-9]+ draws, [0-9]+ skipped"
      FAIL_REGULAR_EXPRESSION "validation: [a-z]+, [1-9][0-9]* errors;error: ")
  elseif(NOT EXISTS "${replay_capture}")
    message(STATUS "DAL: rsx_replay test not registered "
      "(generated/rsx/captures/${DAL_RSX_REPLAY_CAPTURE}.dalcap not found)")
  endif()
endfunction()
