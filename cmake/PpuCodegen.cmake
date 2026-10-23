include_guard(GLOBAL)

option(TOX2_PPU_WERROR "Treat warnings in generated PPU code as errors" ON)

# Options for every target that compiles generated PPU code or code that mirrors it. Target
# COMPILE_OPTIONS follow CMAKE_C_FLAGS_<CONFIG>, so the level (-O1 unless a second argument names
# another) overrides Debug's -O0.
function(tox2_ppu_codegen_options target)
  set(opt_level -O1)
  if(ARGC GREATER 1)
    set(opt_level ${ARGV1})
  endif()
  target_compile_options(${target} PRIVATE
    ${opt_level}
    ${TOX2_ARCH_FLAGS}
    -ffp-contract=off
    -fpatchable-function-entry=8
    -Wall
    -Wextra)
  if(TOX2_PPU_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
endfunction()

# Hand-written replacements of recompiled guest functions (config/ppu_overrides.json lists their entries).
# They are an OBJECT library so that the definitions of fn_<addr> are linked whether or not anything
# in an archive references them, and so that no archive order matters. The unit test links them against
# fakes and needs no generated tree. Call after tox2_runtime_core is defined and before
# tox2_ppu_add_generated_tree.
set(TOX2_PPU_OVERRIDE_SOURCES runtime/src/ppu_overrides/atrac_stream.c)

function(tox2_ppu_add_overrides)
  add_library(tox2_ppu_overrides OBJECT ${TOX2_PPU_OVERRIDE_SOURCES})
  tox2_ppu_codegen_options(tox2_ppu_overrides)

  add_executable(tox2_ppu_overrides_tests
    runtime/tests/ppu_overrides_test.cpp
    $<TARGET_OBJECTS:tox2_ppu_overrides>)
  target_compile_options(tox2_ppu_overrides_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_ppu_overrides_tests PRIVATE tox2_runtime_core)

  add_test(NAME ppu_overrides COMMAND tox2_ppu_overrides_tests)
  set_tests_properties(ppu_overrides PROPERTIES
    PASS_REGULAR_EXPRESSION "ppu overrides tests: [0-9]+ passed, 0 failed")
endfunction()

# Adds the generated PPU tree and its link test when TOX2_PPU_GENERATED_DIR is set. Call after
# tox2_ppu_stub is defined. tox2_ppu_generated links no hook library; each consumer picks the stub
# or the real runtime.
set(TOX2_PPU_GENERATED_DIR "" CACHE PATH "Output directory of 'ps3tool emit' (empty: no generated code)")
set(TOX2_GAME_DIR "" CACHE PATH "Game directory used by the game_boot test (empty: the test is not registered)")

function(tox2_ppu_add_generated_tree)
  if(TOX2_PPU_GENERATED_DIR STREQUAL "")
    message(STATUS "TOX2: generated PPU code not configured (TOX2_PPU_GENERATED_DIR is empty)")
    return()
  endif()

  cmake_path(ABSOLUTE_PATH TOX2_PPU_GENERATED_DIR BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
             OUTPUT_VARIABLE generated_dir)
  set(sources_file "${generated_dir}/sources.cmake")
  if(NOT EXISTS "${sources_file}")
    message(FATAL_ERROR
      "TOX2: ${sources_file} not found. Generate it with: "
      "python tools/ps3tool.py emit <EBOOT.elf> --out ${TOX2_PPU_GENERATED_DIR}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sources_file}")
  include("${sources_file}")

  # libsre is part of the program: the game's cellSpurs imports are bound to its recompiled exports.
  set(libsre_sources_file "${generated_dir}/libsre/libsre_sources.cmake")
  if(NOT EXISTS "${libsre_sources_file}")
    message(FATAL_ERROR
      "TOX2: ${libsre_sources_file} not found. libsre.reloc.elf comes from a build with -Pup. Generate it with: "
      "python tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json "
      "--symbol-prefix libsre --chunks 4 --out ${TOX2_PPU_GENERATED_DIR}/libsre --max-unhandled 0")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${libsre_sources_file}")
  include("${libsre_sources_file}")
  message(STATUS "TOX2: generated PPU code from ${generated_dir} (with libsre)")

  # The SPU tables are part of the program too: the game's SPURS tasks are recompiled SPU code.
  set(spu_sources_file "${generated_dir}/spu/spu_sources.cmake")
  if(NOT EXISTS "${spu_sources_file}")
    message(FATAL_ERROR
      "TOX2: ${spu_sources_file} not found. Generate it with: "
      "python tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf "
      "--libsre generated/inputs/libsre.reloc.elf --out ${TOX2_PPU_GENERATED_DIR}/spu --max-unhandled 0")
  endif()

  add_library(tox2_ppu_generated STATIC ${TOX2_PPU_GENERATED_SOURCES} ${TOX2_LIBSRE_PPU_GENERATED_SOURCES})
  tox2_ppu_codegen_options(tox2_ppu_generated)
  target_include_directories(tox2_ppu_generated PUBLIC "${generated_dir}" "${generated_dir}/libsre")

  add_executable(tox2_ppu_link_test runtime/tests/ppu_link_test.cpp)
  target_compile_options(tox2_ppu_link_test PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  # The stub runtimes only supply the hooks of the generated code; the SPU library is defined by
  # tox2_spu_add_generated_tree, which runs after this function.
  target_link_libraries(tox2_ppu_link_test PRIVATE tox2_ppu_generated tox2_spu_generated tox2_ppu_stub tox2_spu_stub)
  # The objects define the overridden fn_<addr> symbols; the generated tree only holds the renamed bodies.
  target_sources(tox2_ppu_link_test PRIVATE $<TARGET_OBJECTS:tox2_ppu_overrides>)

  add_test(NAME ppu_link COMMAND tox2_ppu_link_test)
  set_tests_properties(ppu_link PROPERTIES PASS_REGULAR_EXPRESSION "ppu link ok")

  # Only game_main.cpp references the generated tables, so the runtime core builds without them.
  # The runtime core supplies every hook; neither stub may be linked here (duplicate symbols).
  add_executable(tox2_game runtime/src/game_main.cpp)
  target_compile_options(tox2_game PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_game PRIVATE tox2_ppu_generated tox2_spu_generated tox2_runtime_core tox2_audio_wasapi tox2_input_gameinput)
  target_sources(tox2_game PRIVATE $<TARGET_OBJECTS:tox2_ppu_overrides>)

  # Pins how far the game gets; updated by whoever moves the milestone.
  include("${CMAKE_SOURCE_DIR}/cmake/game_boot.cmake")

  if(TOX2_EBOOT_ELF STREQUAL "" OR TOX2_GAME_DIR STREQUAL "" OR TOX2_PUP STREQUAL "")
    message(STATUS "TOX2: game_boot test not registered (TOX2_EBOOT_ELF, TOX2_GAME_DIR or TOX2_PUP is empty)")
  else()
    separate_arguments(boot_args UNIX_COMMAND "${TOX2_GAME_BOOT_ARGS}")

    add_test(NAME game_boot
      COMMAND tox2_game --game-dir "${TOX2_GAME_DIR}" --elf "${TOX2_EBOOT_ELF}"
              --libsre "${TOX2_INPUTS_DIR}/libsre.reloc.elf" ${boot_args})
    set_tests_properties(game_boot PROPERTIES
      TIMEOUT 60
      # Only the milestone is asserted: while boot HLE is incomplete the run is expected to stop
      # (often fatally) somewhere after it, and a stop before it leaves the pattern unmatched.
      PASS_REGULAR_EXPRESSION "${TOX2_GAME_BOOT_MILESTONE}")
  endif()
endfunction()

# Semantics harness: hand-written cases are generated into C (tools/tests/ppu_semantics), compiled
# like emitted code and run natively. Needs Python3_EXECUTABLE and tox2_ppu_stub.
function(tox2_ppu_add_semantics_harness)
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
  add_custom_target(tox2_ppu_semantics_gen DEPENDS "${gen_stamp}")

  add_executable(tox2_ppu_semantics
    runtime/tests/ppu_semantics_main.c
    "${gen_dir}/ppu_semantics_cases.c")
  set_source_files_properties("${gen_dir}/ppu_semantics_cases.c" PROPERTIES GENERATED TRUE)
  add_dependencies(tox2_ppu_semantics tox2_ppu_semantics_gen)
  tox2_ppu_codegen_options(tox2_ppu_semantics)
  target_include_directories(tox2_ppu_semantics PRIVATE "${gen_dir}")
  target_link_libraries(tox2_ppu_semantics PRIVATE tox2_ppu_stub)

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
" TOX2_HAVE_UBSAN_TRAP_MUSTTAIL)
  cmake_pop_check_state()
  if(TOX2_HAVE_UBSAN_TRAP_MUSTTAIL)
    target_compile_options(tox2_ppu_semantics PRIVATE
      -fsanitize=undefined -fsanitize-trap=undefined)
    message(STATUS "TOX2: semantics harness UBSan-trap: on")
  else()
    message(STATUS "TOX2: semantics harness UBSan-trap: off (probe failed)")
  endif()

  add_test(NAME ppu_semantics COMMAND tox2_ppu_semantics)
  set_tests_properties(ppu_semantics PROPERTIES
    PASS_REGULAR_EXPRESSION "ppu semantics: [0-9]+ passed, 0 failed"
    TIMEOUT 120)
endfunction()
