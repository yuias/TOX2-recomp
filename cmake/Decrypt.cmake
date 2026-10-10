include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/rpcs3_keys.cmake")

set(TOX2_EBOOT_BIN "" CACHE FILEPATH
  "Disc PS3_GAME/USRDIR/EBOOT.BIN to decrypt into generated/inputs/EBOOT.elf (empty: not decrypted)")
set(TOX2_PUP "" CACHE FILEPATH
  "PS3UPDAT.PUP to extract libsre.prx from (empty: not extracted)")

# Shared by both presets: the decrypted content is identical.
set(TOX2_INPUTS_DIR "${CMAKE_SOURCE_DIR}/generated/inputs")

# tox2_decrypt: the offline decryption tool for EBOOT.BIN and the firmware's libsre.sprx, its synthetic
# unit tests, and the build steps that produce generated/inputs. Call after tox2_runtime_add_core() and
# enable_testing().
function(tox2_decrypt_add)
  set(decrypt_options -march=x86-64-v3 -Wall -Wextra)

  add_library(tox2_decrypt_lib STATIC
    tools/tox2_decrypt/cli.cpp
    tools/tox2_decrypt/crypto.cpp
    tools/tox2_decrypt/keys.cpp
    tools/tox2_decrypt/sce.cpp
    tools/tox2_decrypt/pup.cpp
    tools/tox2_decrypt/self.cpp)
  target_compile_options(tox2_decrypt_lib PRIVATE ${decrypt_options})
  target_include_directories(tox2_decrypt_lib PUBLIC tools/tox2_decrypt)
  target_link_libraries(tox2_decrypt_lib PUBLIC bcrypt zlibstatic)

  add_executable(tox2_decrypt tools/tox2_decrypt/main.cpp)
  target_compile_options(tox2_decrypt PRIVATE ${decrypt_options})
  target_link_libraries(tox2_decrypt PRIVATE tox2_decrypt_lib)

  add_executable(tox2_decrypt_tests
    tools/tox2_decrypt/tests/test_main.cpp
    tools/tox2_decrypt/tests/cli_test.cpp
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

  if(TOX2_EBOOT_BIN STREQUAL "" AND TOX2_PUP STREQUAL "")
    message(STATUS "TOX2: input decryption not configured (TOX2_EBOOT_BIN and TOX2_PUP are empty)")
    return()
  endif()

  tox2_resolve_rpcs3_keys()

  # The pins are read by CMake and passed as arguments, so the tool needs no JSON parser.
  set(target_json "${CMAKE_SOURCE_DIR}/config/target.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${target_json}")
  file(READ "${target_json}" target_text)
  string(JSON eboot_bin_sha256 GET "${target_text}" eboot_bin sha256)
  string(JSON eboot_elf_size GET "${target_text}" eboot_elf size)
  string(JSON eboot_elf_sha256 GET "${target_text}" eboot_elf sha256)
  string(JSON libsre_size GET "${target_text}" libsre_prx size)
  string(JSON libsre_sha256 GET "${target_text}" libsre_prx sha256)
  string(JSON libsre_firmware GET "${target_text}" libsre_prx firmware)

  set(key_files "${TOX2_RPCS3_KEYS_DIR}/key_vault.h" "${TOX2_RPCS3_KEYS_DIR}/key_vault.cpp")
  set(libsre_path "dev_flash/sys/external/libsre.sprx")
  set(input_outputs)
  set(test_dir "${CMAKE_BINARY_DIR}/decrypt_test")

  if(NOT TOX2_EBOOT_BIN STREQUAL "")
    set(eboot_args self --keys "${TOX2_RPCS3_KEYS_DIR}" --in "${TOX2_EBOOT_BIN}"
      --expect-size ${eboot_elf_size} --expect-sha256 ${eboot_elf_sha256}
      --expect-input-sha256 ${eboot_bin_sha256})
    add_custom_command(OUTPUT "${TOX2_INPUTS_DIR}/EBOOT.elf"
      COMMAND tox2_decrypt ${eboot_args} --out "${TOX2_INPUTS_DIR}/EBOOT.elf"
      DEPENDS tox2_decrypt "${TOX2_EBOOT_BIN}" ${key_files}
      COMMENT "Decrypting EBOOT.BIN to generated/inputs/EBOOT.elf"
      VERBATIM)
    list(APPEND input_outputs "${TOX2_INPUTS_DIR}/EBOOT.elf")

    add_test(NAME decrypt_eboot COMMAND tox2_decrypt ${eboot_args} --out "${test_dir}/EBOOT.elf")
    set_tests_properties(decrypt_eboot PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${eboot_elf_sha256}: matches the pin"
      TIMEOUT 120)
  endif()

  if(NOT TOX2_PUP STREQUAL "")
    set(libsre_args pup --keys "${TOX2_RPCS3_KEYS_DIR}" --in "${TOX2_PUP}" --path ${libsre_path}
      --firmware ${libsre_firmware} --expect-size ${libsre_size} --expect-sha256 ${libsre_sha256})
    add_custom_command(OUTPUT "${TOX2_INPUTS_DIR}/libsre.prx"
      COMMAND tox2_decrypt ${libsre_args} --out "${TOX2_INPUTS_DIR}/libsre.prx"
      DEPENDS tox2_decrypt "${TOX2_PUP}" ${key_files}
      COMMENT "Extracting libsre.prx from the PUP to generated/inputs/libsre.prx"
      VERBATIM)
    list(APPEND input_outputs "${TOX2_INPUTS_DIR}/libsre.prx")

    add_test(NAME decrypt_libsre COMMAND tox2_decrypt ${libsre_args} --out "${test_dir}/libsre.prx")
    set_tests_properties(decrypt_libsre PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${libsre_sha256}: matches the pin"
      TIMEOUT 120)
  endif()

  add_custom_target(tox2_inputs ALL DEPENDS ${input_outputs})
endfunction()
