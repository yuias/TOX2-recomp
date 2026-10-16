# tox2_bench: fixed-input benchmarks in thread cycles (tools/tox2_bench). Built when the runtime core and the
# generated SPU tree exist. Call after tox2_runtime_add_core and tox2_spu_add_generated_tree.
function(tox2_bench_add)
  if(NOT TARGET tox2_spu_generated OR NOT TARGET tox2_runtime_core)
    return()
  endif()

  add_executable(tox2_bench tools/tox2_bench/main.cpp)
  target_compile_options(tox2_bench PRIVATE -march=x86-64-v3 -Wall -Wextra)
  # The fixtures shared with the correctness tests live next to them.
  target_include_directories(tox2_bench PRIVATE runtime/tests)
  target_link_libraries(tox2_bench PRIVATE tox2_spu_generated tox2_runtime_core)

  # The LZMA case reads a real item, so it is registered under the conditions of spu_lzma_inflate.
  if(NOT TOX2_GAME_DIR STREQUAL "" AND NOT TOX2_EBOOT_ELF STREQUAL "")
    add_test(NAME bench_spu_lzma
      COMMAND tox2_bench spu-lzma --game-dir "${TOX2_GAME_DIR}" --elf "${TOX2_EBOOT_ELF}"
              --iterations 2 --warmup 1)
    set_tests_properties(bench_spu_lzma PROPERTIES
      PASS_REGULAR_EXPRESSION "bench spu-lzma: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)
  endif()

  add_test(NAME bench_spu_dma_ls COMMAND tox2_bench spu-dma-ls --iterations 2 --warmup 1)
  set_tests_properties(bench_spu_dma_ls PROPERTIES
    PASS_REGULAR_EXPRESSION "bench spu-dma-ls: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsv_wake COMMAND tox2_bench rsv-wake --rounds 200 --iterations 2 --warmup 1)
  set_tests_properties(bench_rsv_wake PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsv-wake: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)
endfunction()
