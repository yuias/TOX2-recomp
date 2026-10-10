include_guard(GLOBAL)

# tox2_decrypt: the offline decryption tool for EBOOT.BIN and the firmware's libsre.sprx, and its
# synthetic unit tests. Call after tox2_runtime_add_core() and enable_testing().
function(tox2_decrypt_add)
  set(decrypt_options -march=x86-64-v3 -Wall -Wextra)

  add_library(tox2_decrypt_lib STATIC
    tools/tox2_decrypt/crypto.cpp
    tools/tox2_decrypt/keys.cpp
    tools/tox2_decrypt/sce.cpp
    tools/tox2_decrypt/pup.cpp
    tools/tox2_decrypt/self.cpp)
  target_compile_options(tox2_decrypt_lib PRIVATE ${decrypt_options})
  target_include_directories(tox2_decrypt_lib PUBLIC tools/tox2_decrypt)
  target_link_libraries(tox2_decrypt_lib PUBLIC bcrypt zlibstatic)

  add_executable(tox2_decrypt_tests
    tools/tox2_decrypt/tests/test_main.cpp
    tools/tox2_decrypt/tests/crypto_test.cpp
    tools/tox2_decrypt/tests/keys_test.cpp
    tools/tox2_decrypt/tests/sce_test.cpp
    tools/tox2_decrypt/tests/pup_test.cpp
    tools/tox2_decrypt/tests/self_test.cpp)
  target_compile_options(tox2_decrypt_tests PRIVATE ${decrypt_options})
  target_link_libraries(tox2_decrypt_tests PRIVATE tox2_decrypt_lib)

  add_test(NAME decrypt_core COMMAND tox2_decrypt_tests)
  set_tests_properties(decrypt_core PROPERTIES
    PASS_REGULAR_EXPRESSION "decrypt core tests: [0-9]+ passed, 0 failed")
endfunction()
