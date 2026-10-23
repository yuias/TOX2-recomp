include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/ui_font.cmake")

# Dear ImGui comes from a pinned submodule and is built from its core files plus the Vulkan backend
# and, on Windows, the Win32 backend or, on macOS, the Cocoa bridge (no demo window). Vulkan entry
# points resolve through volk, so the backend must not include <vulkan/vulkan.h> prototypes. Nothing
# is fetched or generated at configure time.
function(tox2_overlay_add_imgui)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/imgui/LICENSE.txt)
    message(FATAL_ERROR
      "TOX2: externals/imgui is missing or empty. Run: git submodule update --init")
  endif()

  set(imgui_dir ${CMAKE_SOURCE_DIR}/externals/imgui)
  add_library(imgui STATIC
    ${imgui_dir}/imgui.cpp
    ${imgui_dir}/imgui_draw.cpp
    ${imgui_dir}/imgui_tables.cpp
    ${imgui_dir}/imgui_widgets.cpp
    ${imgui_dir}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_dir} ${imgui_dir}/backends)
  target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_compile_options(imgui PRIVATE ${TOX2_ARCH_FLAGS})
  target_link_libraries(imgui PUBLIC volk)
  if(WIN32)
    target_sources(imgui PRIVATE ${imgui_dir}/backends/imgui_impl_win32.cpp)
    # The game window has no IME and the overlay takes no gamepad input.
    target_compile_definitions(imgui PUBLIC
      IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
      IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)
    target_link_libraries(imgui PRIVATE dwmapi gdi32)
  elseif(APPLE)
    # imgui_impl_osx.mm is not a source of its own: the bridge includes it to reach the backend's static
    # event handler. The backend needs ARC, and it links GameController unconditionally.
    target_sources(imgui PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src/overlay/imgui_osx_bridge.mm)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/runtime/src/overlay/imgui_osx_bridge.mm
      PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(imgui PUBLIC "-framework Cocoa" "-framework GameController")
  endif()
endfunction()

# The overlay libraries. Call after tox2_render_add(), which defines volk and tox2_render and enables
# testing. tox2_render stays free of ImGui; only this library links it.
function(tox2_overlay_add)
  tox2_overlay_add_imgui()

  add_library(tox2_overlay STATIC
    runtime/src/overlay/debug_overlay.cpp
    runtime/src/overlay/overlay_model.cpp
    runtime/src/overlay/overlay_panels.cpp
    runtime/src/overlay/overlay_sources.cpp
    runtime/src/overlay/save_dialog_overlay.cpp
    runtime/src/overlay/ui_font.cpp)
  if(WIN32)
    target_sources(tox2_overlay PRIVATE runtime/src/overlay/overlay_platform_win32.cpp)
  elseif(APPLE)
    target_sources(tox2_overlay PRIVATE runtime/src/overlay/overlay_platform_macos.mm)
    target_link_libraries(tox2_overlay PRIVATE "-framework Cocoa")
  endif()
  target_compile_options(tox2_overlay PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_include_directories(tox2_overlay PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  target_link_libraries(tox2_overlay PUBLIC imgui tox2_render)

  # Needs no GPU for the model tests; the Vulkan parts skip without a device.
  add_executable(tox2_overlay_tests runtime/tests/overlay_test.cpp)
  target_compile_options(tox2_overlay_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_overlay_tests PRIVATE tox2_overlay)

  add_test(NAME overlay_core COMMAND tox2_overlay_tests)
  set_tests_properties(overlay_core PROPERTIES
    PASS_REGULAR_EXPRESSION "overlay tests: [0-9]+ passed, 0 failed")

  # Fallback font of the save dialog, copied next to the executables.
  tox2_ui_fonts_add()
endfunction()
