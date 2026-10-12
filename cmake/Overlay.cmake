include_guard(GLOBAL)

# Dear ImGui comes from a pinned submodule and is built from its core files plus the Win32 and Vulkan
# backends (no demo window). Vulkan entry points resolve through volk, so the backend must not
# include <vulkan/vulkan.h> prototypes. Nothing is fetched or generated at configure time.
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
    ${imgui_dir}/backends/imgui_impl_win32.cpp
    ${imgui_dir}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_dir} ${imgui_dir}/backends)
  # The game window has no IME and the overlay takes no gamepad input.
  target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)
  target_compile_options(imgui PRIVATE -march=x86-64-v3)
  target_link_libraries(imgui PUBLIC volk PRIVATE dwmapi gdi32)
endfunction()

# The overlay libraries. Call after tox2_render_add(), which defines volk and tox2_render.
function(tox2_overlay_add)
  tox2_overlay_add_imgui()
endfunction()
