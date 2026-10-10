include_guard(GLOBAL)

# tox2_decrypt reads its keys from two RPCS3 source files. They are not committed here: configuration
# downloads them from a pinned RPCS3 commit and checks their SHA-256. Only key constants are read from
# these GPL-2.0 files, as data; no RPCS3 code is compiled or linked.
set(TOX2_RPCS3_KEYS_COMMIT "f9b8c209a3800c1865bcfeade611f65f5237138c")
set(TOX2_RPCS3_KEY_VAULT_H_SHA256 "d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf")
set(TOX2_RPCS3_KEY_VAULT_CPP_SHA256 "a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7")
set(TOX2_RPCS3_KEYS_URL_BASE "https://raw.githubusercontent.com/RPCS3/rpcs3/${TOX2_RPCS3_KEYS_COMMIT}/rpcs3/Crypto")

set(TOX2_RPCS3_CRYPTO_DIR "" CACHE PATH
  "Directory with key_vault.h and key_vault.cpp of an RPCS3 checkout (rpcs3/Crypto); empty: download the pinned copies")

# Sets TOX2_RPCS3_KEYS_DIR in the caller to the directory holding key_vault.h and key_vault.cpp.
# Call only when the keys are needed: every failure is fatal.
function(tox2_resolve_rpcs3_keys)
  set(key_files key_vault.h key_vault.cpp)
  set(key_pins "${TOX2_RPCS3_KEY_VAULT_H_SHA256}" "${TOX2_RPCS3_KEY_VAULT_CPP_SHA256}")
  set(hint "Set TOX2_RPCS3_CRYPTO_DIR (build.ps1 -Rpcs3CryptoDir) to rpcs3/Crypto of a local RPCS3 checkout to build without the download.")

  if(NOT TOX2_RPCS3_CRYPTO_DIR STREQUAL "")
    foreach(name pin IN ZIP_LISTS key_files key_pins)
      set(file "${TOX2_RPCS3_CRYPTO_DIR}/${name}")
      if(NOT EXISTS "${file}")
        message(FATAL_ERROR "TOX2: ${file} not found (TOX2_RPCS3_CRYPTO_DIR must hold key_vault.h and key_vault.cpp).")
      endif()
      # A different RPCS3 commit normally has the same entries; the output pins catch a real problem.
      file(SHA256 "${file}" hash)
      if(NOT hash STREQUAL pin)
        message(STATUS "TOX2: ${name} in TOX2_RPCS3_CRYPTO_DIR differs from the pinned RPCS3 ${TOX2_RPCS3_KEYS_COMMIT}")
      endif()
    endforeach()
    set(TOX2_RPCS3_KEYS_DIR "${TOX2_RPCS3_CRYPTO_DIR}" PARENT_SCOPE)
    return()
  endif()

  set(keys_dir "${CMAKE_SOURCE_DIR}/generated/deps/rpcs3/${TOX2_RPCS3_KEYS_COMMIT}")
  foreach(name pin IN ZIP_LISTS key_files key_pins)
    set(file "${keys_dir}/${name}")
    set(hash "")
    if(EXISTS "${file}")
      file(SHA256 "${file}" hash)
    endif()
    if(NOT hash STREQUAL pin)
      message(STATUS "TOX2: downloading ${name} from RPCS3 ${TOX2_RPCS3_KEYS_COMMIT}")
      file(DOWNLOAD "${TOX2_RPCS3_KEYS_URL_BASE}/${name}" "${file}.part"
        EXPECTED_HASH SHA256=${pin}
        STATUS status TLS_VERIFY ON)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        file(REMOVE "${file}.part")
        list(GET status 1 reason)
        message(FATAL_ERROR "TOX2: ${name} download failed (${reason}). ${hint}")
      endif()
      file(RENAME "${file}.part" "${file}")
    endif()
  endforeach()
  set(TOX2_RPCS3_KEYS_DIR "${keys_dir}" PARENT_SCOPE)
endfunction()
