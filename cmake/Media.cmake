include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake")

# The FFmpeg build is fetched and verified by tools/fetch-ffmpeg.ps1 into generated/deps/ffmpeg.
# Only its headers are used at build time; the DLLs are loaded at run time (see
# runtime/src/media/ffmpeg_api.h), so no import library is linked. Needs a successful fetch, which
# leaves a marker holding the SHA-256 of the pinned zip.
function(dal_media_find_ffmpeg)
  set(DAL_FFMPEG_DIR "" CACHE PATH
    "Extracted FFmpeg build (default: generated/deps/ffmpeg/<pinned asset name>)")
  if("${DAL_FFMPEG_DIR}" STREQUAL "")
    string(REGEX REPLACE "\\.zip$" "" asset_stem "${DAL_FFMPEG_ASSET}")
    set(ffmpeg_dir "${CMAKE_SOURCE_DIR}/generated/deps/ffmpeg/${asset_stem}")
  else()
    set(ffmpeg_dir "${DAL_FFMPEG_DIR}")
  endif()

  set(marker "${ffmpeg_dir}/.verified-sha256")
  # A fetch after this configure must re-run it.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
  set(fetch_hint "Run: pwsh -NoProfile -File tools/fetch-ffmpeg.ps1")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "DAL: FFmpeg is not fetched (${marker} not found). ${fetch_hint}")
  endif()
  file(READ "${marker}" marker_hash)
  string(STRIP "${marker_hash}" marker_hash)
  string(TOLOWER "${marker_hash}" marker_hash)
  string(TOLOWER "${DAL_FFMPEG_SHA256}" pinned_hash)
  if(NOT marker_hash STREQUAL pinned_hash)
    message(FATAL_ERROR
      "DAL: FFmpeg in ${ffmpeg_dir} does not match the pin in cmake/ffmpeg.cmake. ${fetch_hint}")
  endif()
  foreach(path include/libavcodec/avcodec.h LICENSE.txt)
    if(NOT EXISTS "${ffmpeg_dir}/${path}")
      message(FATAL_ERROR "DAL: ${ffmpeg_dir}/${path} is missing. ${fetch_hint} -Force")
    endif()
  endforeach()

  # DLL names carry the library major versions of the pinned build. avcodec imports swresample
  # statically, so it is part of the set although the runtime calls none of its functions.
  set(ffmpeg_dlls "")
  foreach(stem avutil swresample swscale avcodec)
    file(GLOB dll "${ffmpeg_dir}/bin/${stem}-*.dll")
    list(LENGTH dll dll_count)
    if(NOT dll_count EQUAL 1)
      message(FATAL_ERROR "DAL: expected one ${stem}-*.dll in ${ffmpeg_dir}/bin. ${fetch_hint} -Force")
    endif()
    list(APPEND ffmpeg_dlls "${dll}")
  endforeach()

  set(DAL_FFMPEG_RESOLVED_DIR "${ffmpeg_dir}" PARENT_SCOPE)
  set(DAL_FFMPEG_DLLS "${ffmpeg_dlls}" PARENT_SCOPE)
  message(STATUS "DAL: FFmpeg: ${ffmpeg_dir}")
endfunction()

# The media library: FFmpeg access for movie playback. dal_runtime_core never sees FFmpeg headers.
# Call after dal_runtime_add_core() and enable_testing().
function(dal_media_add)
  dal_media_find_ffmpeg()

  add_library(dal_media STATIC runtime/src/media/ffmpeg_api.cpp runtime/src/media/ffmpeg_codecs.cpp)
  target_compile_options(dal_media PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_media PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  # SYSTEM keeps the FFmpeg headers out of our warnings; nothing links against FFmpeg itself.
  target_include_directories(dal_media SYSTEM PUBLIC "${DAL_FFMPEG_RESOLVED_DIR}/include")
  target_link_libraries(dal_media PUBLIC dal_runtime_core)

  # The runtime loads the DLLs from the directory of the executable. The licence text and the
  # notice travel with them.
  set(bin_dir "${CMAKE_BINARY_DIR}/bin")
  set(licence_dir "${bin_dir}/licenses/ffmpeg")
  set(copied "")
  foreach(dll ${DAL_FFMPEG_DLLS})
    get_filename_component(dll_name "${dll}" NAME)
    add_custom_command(OUTPUT "${bin_dir}/${dll_name}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${bin_dir}/${dll_name}"
      DEPENDS "${dll}"
      VERBATIM)
    list(APPEND copied "${bin_dir}/${dll_name}")
  endforeach()
  add_custom_command(OUTPUT "${licence_dir}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${DAL_FFMPEG_RESOLVED_DIR}/LICENSE.txt" "${licence_dir}/LICENSE.txt"
    DEPENDS "${DAL_FFMPEG_RESOLVED_DIR}/LICENSE.txt"
    VERBATIM)
  add_custom_command(OUTPUT "${licence_dir}/FFMPEG-NOTICE.txt"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${licence_dir}/FFMPEG-NOTICE.txt"
    DEPENDS "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    VERBATIM)
  list(APPEND copied "${licence_dir}/LICENSE.txt" "${licence_dir}/FFMPEG-NOTICE.txt")
  add_custom_target(dal_ffmpeg_runtime ALL DEPENDS ${copied})

  add_executable(dal_media_ffmpeg_tests runtime/tests/media_ffmpeg_test.cpp)
  target_compile_options(dal_media_ffmpeg_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_media_ffmpeg_tests PRIVATE dal_media)
  add_dependencies(dal_media_ffmpeg_tests dal_ffmpeg_runtime)

  add_test(NAME media_ffmpeg COMMAND dal_media_ffmpeg_tests)
  set_tests_properties(media_ffmpeg PROPERTIES
    PASS_REGULAR_EXPRESSION "media ffmpeg tests: [0-9]+ passed, 0 failed")

  # Decodes a movie headless. Writes PNGs only where told. dal_render is defined after this function
  # runs; target names resolve at generate time. It supplies the PNG writer.
  add_executable(dal_moviedump tools/dal_moviedump/main.cpp)
  target_compile_options(dal_moviedump PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_moviedump PRIVATE dal_media dal_render)
  add_dependencies(dal_moviedump dal_ffmpeg_runtime)

  include("${CMAKE_SOURCE_DIR}/cmake/movie_decode.cmake")
  set(movie_path "${DAL_GAME_DIR}/${DAL_MOVIE_DECODE_MOVIE}")
  if("${DAL_GAME_DIR}" STREQUAL "")
    message(STATUS "DAL: movie_decode test not registered (DAL_GAME_DIR is empty)")
  elseif(NOT EXISTS "${movie_path}")
    message(STATUS "DAL: movie_decode test not registered (${DAL_MOVIE_DECODE_MOVIE} not found in the game directory)")
  else()
    set(movie_args "${movie_path}" --frames ${DAL_MOVIE_DECODE_FRAMES}
      --expect-frames ${DAL_MOVIE_DECODE_EXPECTED_FRAMES}
      --audio --expect-audio-samples ${DAL_MOVIE_DECODE_AUDIO_SAMPLES}
      --expect-pcm "${DAL_MOVIE_DECODE_PCM_SHA256}")
    foreach(pin ${DAL_MOVIE_DECODE_YUV_SHA256})
      list(APPEND movie_args --expect-yuv "${pin}")
    endforeach()
    foreach(pin ${DAL_MOVIE_DECODE_RGB_SHA256})
      list(APPEND movie_args --expect-rgb "${pin}")
    endforeach()
    add_test(NAME movie_decode COMMAND dal_moviedump ${movie_args})
    set_tests_properties(movie_decode PROPERTIES PASS_REGULAR_EXPRESSION "moviedump ok")
  endif()

  # dal_audiotool is defined in dal_audio_add(); target names resolve at generate time.
  include("${CMAKE_SOURCE_DIR}/cmake/atrac_decode.cmake")
  set(atrac_archive "${CMAKE_BINARY_DIR}/sdata_cache/${DAL_ATRAC_DECODE_ARCHIVE}")
  # The cache is decrypted at build time (see PpuCodegen.cmake), so it may not exist yet at configure
  # time: the sdata_cache target that produces it also registers the test. An existing cache from an
  # earlier build is enough as well.
  if(NOT EXISTS "${atrac_archive}" AND NOT TARGET sdata_cache)
    message(STATUS "DAL: atrac_decode test not registered (no sdata cache and nothing to build it)")
  else()
    add_test(NAME atrac_decode
      COMMAND dal_audiotool at3 "${atrac_archive}" --index ${DAL_ATRAC_DECODE_INDEX} --hash
        --expect-frames ${DAL_ATRAC_DECODE_FRAMES} --expect-samples ${DAL_ATRAC_DECODE_SAMPLES}
        --expect-pcm "${DAL_ATRAC_DECODE_PCM_SHA256}")
    set_tests_properties(atrac_decode PROPERTIES PASS_REGULAR_EXPRESSION "at3 ok")
  endif()
endfunction()
