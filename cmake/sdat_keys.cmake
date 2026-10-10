include_guard(GLOBAL)

# The .sdat keys are read from RPCS3's key_vault.h. It is not committed here: configuration
# downloads it from a pinned RPCS3 commit and checks its SHA-256. dal_sdat only parses the four
# constants it needs from the file; no RPCS3 code is compiled or linked.
set(DAL_SDAT_KEYS_COMMIT "b8ca6d953f734900c23f2cfbe79a36e598171553")
set(DAL_SDAT_KEYS_SHA256 "d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf")
set(DAL_SDAT_KEYS_URL
  "https://raw.githubusercontent.com/RPCS3/rpcs3/${DAL_SDAT_KEYS_COMMIT}/rpcs3/Crypto/key_vault.h")

set(DAL_RPCS3_CRYPTO_DIR "" CACHE PATH
  "key_vault.h of an RPCS3 checkout, or its directory (empty: download the pinned copy)")

# Sets DAL_SDAT_KEYS in the caller to the key file for dal_sdat --keys, or to "" when there is none.
# A failed download is not fatal: the build then skips the .sdat cache, and the game stops at its
# error dialog for the missing file.
function(dal_resolve_sdat_keys)
  if(NOT DAL_RPCS3_CRYPTO_DIR STREQUAL "")
    set(DAL_SDAT_KEYS "${DAL_RPCS3_CRYPTO_DIR}" PARENT_SCOPE)
    return()
  endif()

  set(keys "${CMAKE_SOURCE_DIR}/generated/deps/rpcs3/${DAL_SDAT_KEYS_COMMIT}/key_vault.h")
  if(EXISTS "${keys}")
    file(SHA256 "${keys}" hash)
  endif()
  if(NOT hash STREQUAL DAL_SDAT_KEYS_SHA256)
    message(STATUS "DAL: downloading key_vault.h from RPCS3 ${DAL_SDAT_KEYS_COMMIT}")
    file(DOWNLOAD "${DAL_SDAT_KEYS_URL}" "${keys}.part"
      EXPECTED_HASH SHA256=${DAL_SDAT_KEYS_SHA256}
      STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${keys}.part")
      list(GET status 1 reason)
      message(WARNING "DAL: key_vault.h download failed (${reason}); the .sdat files are not decrypted. "
        "Set DAL_RPCS3_CRYPTO_DIR to a local copy to build without the download.")
      set(DAL_SDAT_KEYS "" PARENT_SCOPE)
      return()
    endif()
    file(RENAME "${keys}.part" "${keys}")
  endif()
  set(DAL_SDAT_KEYS "${keys}" PARENT_SCOPE)
endfunction()
