include_guard(GLOBAL)

# The value stored for a path option: relative to the config file's directory when the path is inside
# the source or build tree on the same drive, absolute otherwise. The file is git-ignored and
# machine-local, so an absolute path is fine; a relative one keeps working when the checkout moves.
# Forward slashes only, so the JSON needs no backslash handling for paths from this function.
function(tox2_game_config_path out_var path config_dir)
  cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE absolute)
  set(value "${absolute}")
  cmake_path(GET absolute ROOT_NAME path_root)
  cmake_path(GET config_dir ROOT_NAME config_root)
  if(path_root STREQUAL config_root)
    foreach(tree IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
      cmake_path(IS_PREFIX tree "${absolute}" NORMALIZE inside)
      if(inside)
        cmake_path(RELATIVE_PATH absolute BASE_DIRECTORY "${config_dir}" OUTPUT_VARIABLE value)
        break()
      endif()
    endforeach()
  endif()
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# What a stored value points at, for comparing an existing file with the build: absolute against the
# config file's directory, lower case (Windows paths compare case-insensitively), no trailing slash.
function(tox2_game_config_comparable out_var value config_dir)
  cmake_path(ABSOLUTE_PATH value BASE_DIRECTORY "${config_dir}" NORMALIZE OUTPUT_VARIABLE absolute)
  string(REGEX REPLACE "/+$" "" absolute "${absolute}")
  string(TOLOWER "${absolute}" absolute)
  set(${out_var} "${absolute}" PARENT_SCOPE)
endfunction()

# Writes <bin>/config.json, the file tox2_game reads when started without arguments, from the paths
# this build was given. Done at configure time because every input is a cache variable (TOX2_GAME_DIR,
# TOX2_EBOOT_ELF, TOX2_PUP), so it also works for a plain `cmake --preset` without tools/build.ps1.
# An existing file is never modified: it belongs to the user (and to the game's own settings saving).
# Call after TOX2_EBOOT_ELF has its default, when the tox2_game target exists.
function(tox2_game_config_write)
  if(TOX2_GAME_DIR STREQUAL "")
    message(STATUS "TOX2: bin/config.json not written (TOX2_GAME_DIR is empty)")
    return()
  endif()

  set(config_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  set(config_file "${config_dir}/config.json")

  # key -> source path; a key whose source is unknown is left out. hdd_dir is not written: the default
  # build/<preset>/hdd next to bin/ is what the README describes.
  set(keys game_dir)
  set(source_game_dir "${TOX2_GAME_DIR}")
  if(NOT TOX2_EBOOT_ELF STREQUAL "")
    list(APPEND keys elf)
    set(source_elf "${TOX2_EBOOT_ELF}")
  endif()
  if(NOT TOX2_PUP STREQUAL "")
    list(APPEND keys libsre)
    set(source_libsre "${TOX2_INPUTS_DIR}/libsre.reloc.elf")
  endif()

  if(EXISTS "${config_file}")
    file(READ "${config_file}" existing_text)
    string(JSON top_type ERROR_VARIABLE top_error TYPE "${existing_text}")
    if(top_error OR NOT top_type STREQUAL "OBJECT")
      message(STATUS "TOX2: ${config_file} left unchanged: it is not a JSON object")
      return()
    endif()
    set(differing)
    foreach(key IN LISTS keys)
      string(JSON existing_value ERROR_VARIABLE key_error GET "${existing_text}" ${key})
      if(key_error)
        list(APPEND differing "${key} (missing)")
        continue()
      endif()
      tox2_game_config_comparable(existing "${existing_value}" "${config_dir}")
      tox2_game_config_comparable(expected "${source_${key}}" "${config_dir}")
      if(NOT existing STREQUAL expected)
        list(APPEND differing "${key}")
      endif()
    endforeach()
    if(differing)
      list(JOIN differing ", " differing_text)
      message(STATUS "TOX2: ${config_file} left unchanged; it differs from this build in: ${differing_text}")
    endif()
    return()
  endif()

  set(members)
  foreach(key IN LISTS keys)
    tox2_game_config_path(value "${source_${key}}" "${config_dir}")
    # Not needed for paths from Windows, but a path passed by hand may hold anything.
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    list(APPEND members "  \"${key}\": \"${value}\"")
  endforeach()
  # Same two-space layout as the file the game itself rewrites when it saves settings.
  list(JOIN members ",\n" members_text)
  file(MAKE_DIRECTORY "${config_dir}")
  file(WRITE "${config_file}" "{\n${members_text}\n}\n")
  list(JOIN keys ", " keys_text)
  message(STATUS "TOX2: wrote ${config_file} (${keys_text})")
endfunction()
