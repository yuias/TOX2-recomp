include_guard(GLOBAL)

# Fallback font of the save dialog: BIZ UDPGothic (SIL Open Font License 1.1) from a pinned commit of
# googlefonts/morisawa-biz-ud-gothic. The firmware font from tox2_decrypt is preferred at run time;
# this copy only covers a build without the PUP. The files are not committed here: configuration
# downloads them and checks their SHA-256. Unlike the RPCS3 keys, the font is optional, so a failed
# download is a status warning and the build goes on without it.
set(TOX2_UI_FONT_COMMIT "18934af56b9c003ca58c54bffbf226848cb11032")
set(TOX2_UI_FONT_TTF_PATH "fonts/ttf/BIZUDPGothic-Regular.ttf")
set(TOX2_UI_FONT_TTF_SHA256 "258d7156c165f2ff774b6efee637c22c3b950de0d8a10e501137061bc8085d01")
set(TOX2_UI_FONT_OFL_PATH "OFL.txt")
set(TOX2_UI_FONT_OFL_SHA256 "e753d7155d53c747d037a445e584c8ecfca6dd79846db610417e282a736b28bc")

set(TOX2_UI_FONT_URL_BASE
  "https://raw.githubusercontent.com/googlefonts/morisawa-biz-ud-gothic/${TOX2_UI_FONT_COMMIT}" CACHE STRING
  "URL prefix of the pinned BIZ UDPGothic files (the pinned raw GitHub URL; change it only to test the failure path)")
set(TOX2_UI_FONT_FILE "" CACHE FILEPATH
  "A local BIZUDPGothic-Regular.ttf to use instead of the download (a hash that differs from the pin is only reported)")

# Downloads <rel_path> of the pinned commit into <dir> unless a file with the pinned hash is already there.
# Sets <out_var> in the caller to the file, or to "" when it cannot be had.
function(tox2_fetch_ui_font_file out_var dir rel_path pin)
  get_filename_component(name "${rel_path}" NAME)
  set(file "${dir}/${name}")
  set(${out_var} "" PARENT_SCOPE)
  if(EXISTS "${file}")
    file(SHA256 "${file}" hash)
    if(hash STREQUAL pin)
      set(${out_var} "${file}" PARENT_SCOPE)
      return()
    endif()
  endif()
  message(STATUS "TOX2: downloading ${name} for the fallback UI font")
  # EXPECTED_HASH is not used: on a failed transfer it raises a configuration error, which would defeat
  # the warning path.
  string(REGEX REPLACE "/+$" "" url_base "${TOX2_UI_FONT_URL_BASE}")
  file(DOWNLOAD "${url_base}/${rel_path}" "${file}.part"
    INACTIVITY_TIMEOUT 30
    STATUS status TLS_VERIFY ON)
  list(GET status 0 code)
  list(GET status 1 reason)
  if(NOT code EQUAL 0)
    file(REMOVE "${file}.part")
    message(STATUS "TOX2: warning: ${name} download failed (${reason})")
    return()
  endif()
  file(SHA256 "${file}.part" hash)
  if(NOT hash STREQUAL pin)
    file(REMOVE "${file}.part")
    message(STATUS "TOX2: warning: ${name} from ${url_base} has sha256 ${hash}, not the pinned ${pin}")
    return()
  endif()
  file(RENAME "${file}.part" "${file}")
  set(${out_var} "${file}" PARENT_SCOPE)
endfunction()

# Sets TOX2_UI_FONT_RESOLVED (the font file) and TOX2_UI_FONT_LICENCE_RESOLVED (its OFL.txt) in the caller;
# either is empty when unavailable. Never fails the configuration.
function(tox2_resolve_ui_font)
  set(font "")
  set(licence "")
  set(dir "${CMAKE_SOURCE_DIR}/generated/deps/fonts/${TOX2_UI_FONT_COMMIT}")

  if(NOT TOX2_UI_FONT_FILE STREQUAL "")
    if(EXISTS "${TOX2_UI_FONT_FILE}")
      set(font "${TOX2_UI_FONT_FILE}")
      file(SHA256 "${font}" hash)
      if(NOT hash STREQUAL TOX2_UI_FONT_TTF_SHA256)
        message(STATUS "TOX2: TOX2_UI_FONT_FILE differs from the pinned BIZ UDPGothic (sha256 ${hash})")
      endif()
      # The licence must travel with the font; take the pinned copy when it can be had.
      tox2_fetch_ui_font_file(licence "${dir}" "${TOX2_UI_FONT_OFL_PATH}" "${TOX2_UI_FONT_OFL_SHA256}")
    else()
      message(STATUS "TOX2: warning: TOX2_UI_FONT_FILE ${TOX2_UI_FONT_FILE} not found; trying the download")
    endif()
  endif()

  if(font STREQUAL "")
    # The font and its licence are taken together: a font without its licence text is not copied.
    tox2_fetch_ui_font_file(font "${dir}" "${TOX2_UI_FONT_TTF_PATH}" "${TOX2_UI_FONT_TTF_SHA256}")
    if(NOT font STREQUAL "")
      tox2_fetch_ui_font_file(licence "${dir}" "${TOX2_UI_FONT_OFL_PATH}" "${TOX2_UI_FONT_OFL_SHA256}")
      if(licence STREQUAL "")
        set(font "")
      endif()
    endif()
  endif()

  if(font STREQUAL "")
    message(STATUS "TOX2: fallback UI font: not available (the save dialog uses the firmware font if the PUP is "
                   "configured, else ImGui's default font and Japanese shows as '?'; see docs/building.md)")
  else()
    message(STATUS "TOX2: fallback UI font: ${font}")
  endif()
  set(TOX2_UI_FONT_RESOLVED "${font}" PARENT_SCOPE)
  set(TOX2_UI_FONT_LICENCE_RESOLVED "${licence}" PARENT_SCOPE)
endfunction()

# Adds the ALL target tox2_ui_fonts that copies the font to bin/fonts/ and its licence to
# bin/licenses/biz-udgothic/. Without a font the target is not created.
function(tox2_ui_fonts_add)
  tox2_resolve_ui_font()
  if(TOX2_UI_FONT_RESOLVED STREQUAL "")
    message(STATUS "TOX2: tox2_ui_fonts skipped (no fallback UI font)")
    return()
  endif()

  set(bin_dir "${CMAKE_BINARY_DIR}/bin")
  set(font_out "${bin_dir}/fonts/BIZUDPGothic-Regular.ttf")
  set(outputs "${font_out}")
  add_custom_command(OUTPUT "${font_out}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOX2_UI_FONT_RESOLVED}" "${font_out}"
    DEPENDS "${TOX2_UI_FONT_RESOLVED}"
    VERBATIM)
  if(NOT TOX2_UI_FONT_LICENCE_RESOLVED STREQUAL "")
    set(licence_out "${bin_dir}/licenses/biz-udgothic/OFL.txt")
    add_custom_command(OUTPUT "${licence_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}/licenses/biz-udgothic"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOX2_UI_FONT_LICENCE_RESOLVED}" "${licence_out}"
      DEPENDS "${TOX2_UI_FONT_LICENCE_RESOLVED}"
      VERBATIM)
    list(APPEND outputs "${licence_out}")
  endif()
  add_custom_target(tox2_ui_fonts ALL DEPENDS ${outputs})
endfunction()
