include_guard(GLOBAL)

include(CMakePushCheckState)
include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "TOX2: clang is required (C: ${CMAKE_C_COMPILER_ID}, C++: ${CMAKE_CXX_COMPILER_ID})")
endif()

# clang-cl would accept the sources but later phases need GNU-style flags.
if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  message(FATAL_ERROR
    "TOX2: clang GNU-style driver required, got frontend '${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}'")
endif()

if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "TOX2: unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

# One CPU baseline for every target; the generated code is built with the same flag.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(TOX2_ARCH_FLAGS "-march=x86-64-v3")
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(TOX2_ARCH_FLAGS "-mcpu=apple-m1")
else()
  message(FATAL_ERROR "TOX2: unsupported processor '${CMAKE_SYSTEM_PROCESSOR}' on ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "TOX2: arch flags: ${TOX2_ARCH_FLAGS}")

# The sources use C++26 library features; a libc++ that lacks them (for example the one in the Apple
# SDK) compiles the headers but fails here at link time.
cmake_push_check_state()
check_cxx_source_compiles([=[
#include <expected>
#include <format>
#include <print>
#include <string>
int main() {
  std::expected<std::string, int> e = std::format("{}", 42);
  std::println("{}", e.value());
  return 0;
}
]=] TOX2_HAVE_CXX_LIBRARY)
cmake_pop_check_state()
if(NOT TOX2_HAVE_CXX_LIBRARY)
  message(FATAL_ERROR "TOX2: the C++ library cannot link C++26 code")
endif()

cmake_push_check_state()
# Without -Werror an unknown attribute is only a warning and the check passes falsely.
set(CMAKE_REQUIRED_FLAGS -Werror)

check_c_source_compiles("
static int g(int x) { return x; }
int f(int x) { __attribute__((musttail)) return g(x); }
int main(void) { return f(0); }
" TOX2_HAVE_MUSTTAIL_C)

check_cxx_source_compiles("
static int g(int x) { return x; }
int f(int x) { [[clang::musttail]] return g(x); }
int main() { return f(0); }
" TOX2_HAVE_MUSTTAIL_CXX)

# Generated code reserves 8 bytes of padding at every function entry for later hooking.
set(CMAKE_REQUIRED_FLAGS "-Werror -fpatchable-function-entry=8")
check_c_source_compiles("
int f(int x) { return x; }
int main(void) { return f(0); }
" TOX2_HAVE_PATCHABLE_ENTRY)
cmake_pop_check_state()

if(NOT TOX2_HAVE_PATCHABLE_ENTRY)
  message(FATAL_ERROR "TOX2: -fpatchable-function-entry=8 is not supported by this compiler/target")
endif()
message(STATUS "TOX2: -fpatchable-function-entry=8 supported")

if(NOT TOX2_HAVE_MUSTTAIL_C OR NOT TOX2_HAVE_MUSTTAIL_CXX)
  message(FATAL_ERROR
    "TOX2: musttail unsupported (C: ${TOX2_HAVE_MUSTTAIL_C}, C++: ${TOX2_HAVE_MUSTTAIL_CXX})")
endif()

message(STATUS "TOX2: musttail supported (C, C++)")
