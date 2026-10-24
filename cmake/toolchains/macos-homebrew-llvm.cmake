# Toolchain for macOS with Homebrew LLVM (brew install llvm). Apple's own clang is rejected by
# cmake/CompilerChecks.cmake, so the compilers come from the LLVM prefix.
#
# The prefix is TOX2_LLVM_PREFIX (cache variable or environment), else `brew --prefix llvm`.

if(NOT TOX2_LLVM_PREFIX AND DEFINED ENV{TOX2_LLVM_PREFIX})
  set(TOX2_LLVM_PREFIX "$ENV{TOX2_LLVM_PREFIX}")
endif()

if(NOT TOX2_LLVM_PREFIX)
  execute_process(
    COMMAND brew --prefix llvm
    OUTPUT_VARIABLE tox2_brew_llvm_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE tox2_brew_result)
  if(tox2_brew_result EQUAL 0)
    set(TOX2_LLVM_PREFIX "${tox2_brew_llvm_prefix}")
  endif()
endif()

if(NOT TOX2_LLVM_PREFIX OR NOT EXISTS "${TOX2_LLVM_PREFIX}/bin/clang++")
  message(FATAL_ERROR
    "TOX2: LLVM clang not found (prefix '${TOX2_LLVM_PREFIX}'). Install it with `brew install llvm` or "
    "set TOX2_LLVM_PREFIX to a directory that contains bin/clang++.")
endif()

# Cached, and mirrored into the environment so the try_compile projects (which re-read this file)
# do not run brew again.
set(TOX2_LLVM_PREFIX "${TOX2_LLVM_PREFIX}" CACHE PATH "LLVM installation prefix" FORCE)
set(ENV{TOX2_LLVM_PREFIX} "${TOX2_LLVM_PREFIX}")

set(CMAKE_C_COMPILER "${TOX2_LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${TOX2_LLVM_PREFIX}/bin/clang++")
set(CMAKE_OBJCXX_COMPILER "${TOX2_LLVM_PREFIX}/bin/clang++")

# Homebrew binutils can put a GNU ar first on PATH; use the LLVM archiver that matches the compiler.
set(CMAKE_AR "${TOX2_LLVM_PREFIX}/bin/llvm-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${TOX2_LLVM_PREFIX}/bin/llvm-ranlib" CACHE FILEPATH "Archive indexer")

# The C++26 library headers of this clang need the matching Homebrew libc++, not the one in the SDK
# (Homebrew's documented link flags for LLVM).
set(tox2_llvm_link_flags
  "-L${TOX2_LLVM_PREFIX}/lib/c++ -Wl,-rpath,${TOX2_LLVM_PREFIX}/lib/c++ -L${TOX2_LLVM_PREFIX}/lib/unwind -lunwind")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${tox2_llvm_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${tox2_llvm_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${tox2_llvm_link_flags}")
