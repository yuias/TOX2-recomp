include_guard(GLOBAL)

# Contract-header check for generated SPU code: the run-time test, plus object-only translation
# units that parse the headers as C++ and with -ffreestanding. Needs tox2_ppu_codegen_options.
function(tox2_spu_add_header_check)
  add_executable(tox2_spu_header_check runtime/tests/spu_header_check.c)
  tox2_ppu_codegen_options(tox2_spu_header_check)

  add_library(tox2_spu_header_freestanding OBJECT runtime/tests/spu_header_freestanding.c)
  tox2_ppu_codegen_options(tox2_spu_header_freestanding)
  target_compile_options(tox2_spu_header_freestanding PRIVATE -ffreestanding)

  add_library(tox2_spu_header_cxx OBJECT runtime/tests/spu_header_cxx.cpp)
  tox2_ppu_codegen_options(tox2_spu_header_cxx)

  # Built with the check so that a header regression fails the build of the test target.
  add_dependencies(tox2_spu_header_check tox2_spu_header_freestanding tox2_spu_header_cxx)

  add_test(NAME spu_header_check COMMAND tox2_spu_header_check)
  set_tests_properties(spu_header_check PROPERTIES PASS_REGULAR_EXPRESSION "spu header ok")
endfunction()

# Semantics harness: hand-written cases are generated into C (tools/tests/spu_semantics) through the
# real emitter, compiled like emitted code and run natively. The driver supplies every runtime hook,
# so nothing is linked. Needs Python3_EXECUTABLE and tox2_ppu_codegen_options; call it after
# tox2_ppu_add_semantics_harness, which probes TOX2_HAVE_UBSAN_TRAP_MUSTTAIL.
function(tox2_spu_add_semantics_harness)
  set(gen_dir "${CMAKE_BINARY_DIR}/spu_semantics")
  set(gen_stamp "${gen_dir}/generate.stamp")

  # The generator rewrites its outputs only when their content changes, so the custom command is
  # tracked through a stamp instead of the outputs (see tox2_ppu_add_semantics_harness).
  file(GLOB gen_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/ps3recomp/*.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_asm.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_semantics/*.py")
  add_custom_command(
    OUTPUT "${gen_stamp}"
    BYPRODUCTS "${gen_dir}/spu_semantics_cases.h" "${gen_dir}/spu_semantics_cases.c"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND ${Python3_EXECUTABLE} -B -m tests.spu_semantics.gen --out "${gen_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${gen_stamp}"
    DEPENDS ${gen_inputs}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools"
    COMMENT "Generating SPU semantics cases"
    VERBATIM)
  add_custom_target(tox2_spu_semantics_gen DEPENDS "${gen_stamp}")

  add_executable(tox2_spu_semantics
    runtime/tests/spu_semantics_main.c
    "${gen_dir}/spu_semantics_cases.c")
  set_source_files_properties("${gen_dir}/spu_semantics_cases.c" PROPERTIES GENERATED TRUE)
  add_dependencies(tox2_spu_semantics tox2_spu_semantics_gen)
  tox2_ppu_codegen_options(tox2_spu_semantics)
  target_include_directories(tox2_spu_semantics PRIVATE "${gen_dir}")

  if(TOX2_HAVE_UBSAN_TRAP_MUSTTAIL)
    target_compile_options(tox2_spu_semantics PRIVATE
      -fsanitize=undefined -fsanitize-trap=undefined)
  endif()

  add_test(NAME spu_semantics COMMAND tox2_spu_semantics)
  set_tests_properties(spu_semantics PROPERTIES
    PASS_REGULAR_EXPRESSION "spu semantics: [0-9]+ passed, 0 failed"
    TIMEOUT 120)
endfunction()
