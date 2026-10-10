include_guard(GLOBAL)

option(DAL_PPU_WERROR "Treat warnings in generated PPU code as errors" ON)

# Options for every target that compiles generated PPU code or code that mirrors it. Target
# COMPILE_OPTIONS follow CMAKE_C_FLAGS_<CONFIG>, so -O1 overrides Debug's -O0.
function(dal_ppu_codegen_options target)
  target_compile_options(${target} PRIVATE
    -O1
    -march=x86-64-v3
    -ffp-contract=off
    -fpatchable-function-entry=8
    -Wall
    -Wextra)
  if(DAL_PPU_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
endfunction()

# Adds the generated PPU tree and its link test when DAL_PPU_GENERATED_DIR is set. Call after
# dal_ppu_stub is defined. dal_ppu_generated links no hook library; each consumer picks the stub
# or the real runtime.
set(DAL_PPU_GENERATED_DIR "" CACHE PATH "Output directory of 'ps3tool emit' (empty: no generated code)")
set(DAL_GAME_DIR "" CACHE PATH "Game directory used by the game_boot test (empty: the test is not registered)")

function(dal_ppu_add_generated_tree)
  if(DAL_PPU_GENERATED_DIR STREQUAL "")
    message(STATUS "DAL: generated PPU code not configured (DAL_PPU_GENERATED_DIR is empty)")
    return()
  endif()

  cmake_path(ABSOLUTE_PATH DAL_PPU_GENERATED_DIR BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
             OUTPUT_VARIABLE generated_dir)
  set(sources_file "${generated_dir}/sources.cmake")
  if(NOT EXISTS "${sources_file}")
    message(FATAL_ERROR
      "DAL: ${sources_file} not found. Generate it with: "
      "python tools/ps3tool.py emit <EBOOT.elf> --out ${DAL_PPU_GENERATED_DIR}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sources_file}")
  include("${sources_file}")
  message(STATUS "DAL: generated PPU code from ${generated_dir}")

  add_library(dal_ppu_generated STATIC ${DAL_PPU_GENERATED_SOURCES})
  dal_ppu_codegen_options(dal_ppu_generated)
  target_include_directories(dal_ppu_generated PUBLIC "${generated_dir}")

  add_executable(dal_ppu_link_test runtime/tests/ppu_link_test.cpp)
  target_compile_options(dal_ppu_link_test PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_ppu_link_test PRIVATE dal_ppu_generated dal_ppu_stub)

  add_test(NAME ppu_link COMMAND dal_ppu_link_test)
  set_tests_properties(ppu_link PROPERTIES PASS_REGULAR_EXPRESSION "ppu link ok")

  # Only game_main.cpp references the generated tables, so the runtime core builds without them.
  # The runtime core supplies every hook; the stub must not be linked here (duplicate symbols).
  add_executable(dal_game runtime/src/game_main.cpp)
  target_compile_options(dal_game PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_game PRIVATE dal_ppu_generated dal_runtime_core dal_audio_wasapi dal_input_gameinput)

  # Pins how far the game gets; updated by whoever moves the milestone.
  include("${CMAKE_SOURCE_DIR}/cmake/game_boot.cmake")

  if(DAL_EBOOT_ELF STREQUAL "" OR DAL_GAME_DIR STREQUAL "")
    message(STATUS "DAL: game_boot test not registered (DAL_EBOOT_ELF or DAL_GAME_DIR is empty)")
  else()
    separate_arguments(boot_args UNIX_COMMAND "${DAL_GAME_BOOT_ARGS}")

    add_test(NAME game_boot
      COMMAND dal_game --game-dir "${DAL_GAME_DIR}" --elf "${DAL_EBOOT_ELF}" ${boot_args})
    set_tests_properties(game_boot PROPERTIES
      TIMEOUT 60
      # Only the milestone is asserted: while boot HLE is incomplete the run is expected to stop
      # (often fatally) somewhere after it, and a stop before it leaves the pattern unmatched.
      PASS_REGULAR_EXPRESSION "${DAL_GAME_BOOT_MILESTONE}")
  endif()
endfunction()

# Semantics harness: hand-written cases are generated into C (tools/tests/ppu_semantics), compiled
# like emitted code and run natively. Needs Python3_EXECUTABLE and dal_ppu_stub.
function(dal_ppu_add_semantics_harness)
  set(gen_dir "${CMAKE_BINARY_DIR}/ppu_semantics")
  set(gen_stamp "${gen_dir}/generate.stamp")

  # The generator rewrites its outputs only when their content changes, so the custom command is
  # tracked through a stamp instead of the outputs; otherwise touching an input without changing
  # the result would leave the command out of date forever.
  file(GLOB gen_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/ps3recomp/*.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/ppc_asm.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/ppu_semantics/*.py")
  add_custom_command(
    OUTPUT "${gen_stamp}"
    BYPRODUCTS "${gen_dir}/ppu_semantics_cases.h" "${gen_dir}/ppu_semantics_cases.c"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND ${Python3_EXECUTABLE} -B -m tests.ppu_semantics.gen --out "${gen_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${gen_stamp}"
    DEPENDS ${gen_inputs}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools"
    COMMENT "Generating PPU semantics cases"
    VERBATIM)
  add_custom_target(dal_ppu_semantics_gen DEPENDS "${gen_stamp}")

  add_executable(dal_ppu_semantics
    runtime/tests/ppu_semantics_main.c
    "${gen_dir}/ppu_semantics_cases.c")
  set_source_files_properties("${gen_dir}/ppu_semantics_cases.c" PROPERTIES GENERATED TRUE)
  add_dependencies(dal_ppu_semantics dal_ppu_semantics_gen)
  dal_ppu_codegen_options(dal_ppu_semantics)
  target_include_directories(dal_ppu_semantics PRIVATE "${gen_dir}")
  target_link_libraries(dal_ppu_semantics PRIVATE dal_ppu_stub)

  # Instrumentation can make a musttail site uncompilable, so the probe contains one with the
  # generated signature. Trap mode needs no sanitizer runtime library.
  cmake_push_check_state()
  set(CMAKE_REQUIRED_FLAGS
    "-Werror -fsanitize=undefined -fsanitize-trap=undefined -fpatchable-function-entry=8")
  check_c_source_compiles("
typedef struct Ctx { int x; } Ctx;
typedef void Fn(Ctx* __restrict ctx, unsigned char* __restrict base);
void g(Ctx* __restrict ctx, unsigned char* __restrict base) { (void)base; ctx->x += 1; }
void f(Ctx* __restrict ctx, unsigned char* __restrict base) {
    Fn* fn = g;
    __attribute__((musttail)) return fn(ctx, base);
}
int main(void) { return 0; }
" DAL_HAVE_UBSAN_TRAP_MUSTTAIL)
  cmake_pop_check_state()
  if(DAL_HAVE_UBSAN_TRAP_MUSTTAIL)
    target_compile_options(dal_ppu_semantics PRIVATE
      -fsanitize=undefined -fsanitize-trap=undefined)
    message(STATUS "DAL: semantics harness UBSan-trap: on")
  else()
    message(STATUS "DAL: semantics harness UBSan-trap: off (probe failed)")
  endif()

  add_test(NAME ppu_semantics COMMAND dal_ppu_semantics)
  set_tests_properties(ppu_semantics PROPERTIES
    PASS_REGULAR_EXPRESSION "ppu semantics: [0-9]+ passed, 0 failed"
    TIMEOUT 120)
endfunction()
