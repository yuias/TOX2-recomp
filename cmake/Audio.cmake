include_guard(GLOBAL)

# Audio tools and tests. The audio library code itself lives in tox2_runtime_core, which stays free
# of Windows audio and FFmpeg headers. Call after tox2_runtime_add_core() and enable_testing().
function(tox2_audio_add)
  add_executable(tox2_audio_core_tests runtime/tests/audio_core_test.cpp)
  target_compile_options(tox2_audio_core_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_audio_core_tests PRIVATE tox2_runtime_core)

  add_test(NAME audio_core COMMAND tox2_audio_core_tests)
  set_tests_properties(audio_core PROPERTIES
    PASS_REGULAR_EXPRESSION "audio core tests: [0-9]+ passed, 0 failed")

  # The device-thread logic shared by the platform outputs; no platform headers.
  add_library(tox2_audio_stream STATIC runtime/src/audio/stream_output.cpp)
  target_compile_options(tox2_audio_stream PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(tox2_audio_stream PUBLIC tox2_runtime_core)

  if(WIN32)
    # The WASAPI output lives outside tox2_runtime_core so that the core stays free of Windows audio
    # headers.
    add_library(tox2_audio_wasapi STATIC runtime/src/audio/wasapi_output.cpp)
    target_compile_options(tox2_audio_wasapi PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_audio_wasapi PUBLIC tox2_audio_stream ole32 avrt)

    # Opens the default render device; exits 77 (skipped) when the machine has none. The device-loss
    # tests use fake streams and run everywhere.
    add_executable(tox2_audio_wasapi_tests runtime/tests/audio_wasapi_test.cpp)
    target_compile_options(tox2_audio_wasapi_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_audio_wasapi_tests PRIVATE tox2_audio_wasapi)

    add_test(NAME audio_wasapi COMMAND tox2_audio_wasapi_tests)
    # Serial: the timing checks (paced submits, a 250 ms stall) assume the machine is not saturated
    # by other tests.
    set_tests_properties(audio_wasapi PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
  endif()

  if(APPLE)
    # CoreAudio's C APIs only; no Objective-C.
    add_library(tox2_audio_coreaudio STATIC runtime/src/audio/coreaudio_output.cpp)
    target_compile_options(tox2_audio_coreaudio PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_audio_coreaudio PUBLIC tox2_audio_stream
      PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")

    # Opens the default output device; exits 77 (skipped) when the machine has none. The tests over
    # fake streams run everywhere.
    add_executable(tox2_audio_coreaudio_tests runtime/tests/audio_coreaudio_test.cpp)
    target_compile_options(tox2_audio_coreaudio_tests PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
    target_link_libraries(tox2_audio_coreaudio_tests PRIVATE tox2_audio_coreaudio)

    add_test(NAME audio_coreaudio COMMAND tox2_audio_coreaudio_tests)
    # Serial: the timing checks assume the machine is not saturated by other tests.
    set_tests_properties(audio_coreaudio PROPERTIES
      SKIP_RETURN_CODE 77
      RUN_SERIAL TRUE
      PASS_REGULAR_EXPRESSION "audio coreaudio tests: [0-9]+ passed, 0 failed")

    if(TARGET tox2_game)
      target_link_libraries(tox2_game PRIVATE tox2_audio_coreaudio)
    endif()
  endif()

  add_executable(tox2_audiotool tools/tox2_audiotool/main.cpp)
  target_compile_options(tox2_audiotool PRIVATE ${TOX2_ARCH_FLAGS} -Wall -Wextra)
  # tox2_media supplies the FFmpeg ATRAC3plus decoder for `at3`; the DLLs are loaded from the
  # executable's directory.
  target_link_libraries(tox2_audiotool PRIVATE tox2_media tox2_crypto)
  if(WIN32)
    target_link_libraries(tox2_audiotool PRIVATE tox2_audio_wasapi)
  else()
    target_link_libraries(tox2_audiotool PRIVATE tox2_audio_coreaudio)
  endif()
  add_dependencies(tox2_audiotool tox2_ffmpeg_runtime)
endfunction()
