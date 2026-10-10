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
