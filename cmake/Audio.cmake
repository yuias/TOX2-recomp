include_guard(GLOBAL)

# Audio tools and tests. The audio library code itself lives in dal_runtime_core, which stays free
# of Windows audio and FFmpeg headers. Call after dal_runtime_add_core() and enable_testing().
function(dal_audio_add)
  add_executable(dal_audio_core_tests runtime/tests/audio_core_test.cpp)
  target_compile_options(dal_audio_core_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_audio_core_tests PRIVATE dal_runtime_core)

  add_test(NAME audio_core COMMAND dal_audio_core_tests)
  set_tests_properties(audio_core PROPERTIES
    PASS_REGULAR_EXPRESSION "audio core tests: [0-9]+ passed, 0 failed")

  # The WASAPI output lives outside dal_runtime_core so that the core stays free of Windows audio
  # headers.
  add_library(dal_audio_wasapi STATIC runtime/src/audio/wasapi_output.cpp)
  target_compile_options(dal_audio_wasapi PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_audio_wasapi PUBLIC dal_runtime_core ole32 avrt)

  # Opens the default render device; exits 77 (skipped) when the machine has none. The device-loss
  # tests use fake streams and run everywhere.
  add_executable(dal_audio_wasapi_tests runtime/tests/audio_wasapi_test.cpp)
  target_compile_options(dal_audio_wasapi_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_audio_wasapi_tests PRIVATE dal_audio_wasapi)

  add_test(NAME audio_wasapi COMMAND dal_audio_wasapi_tests)
  # Serial: the timing checks (paced submits, a 250 ms stall) assume the machine is not saturated by
  # other tests.
  set_tests_properties(audio_wasapi PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

  add_executable(dal_audiotool tools/dal_audiotool/main.cpp)
  target_compile_options(dal_audiotool PRIVATE -march=x86-64-v3 -Wall -Wextra)
  # dal_media supplies the FFmpeg ATRAC3plus decoder for `at3`; the DLLs are loaded from the
  # executable's directory.
  target_link_libraries(dal_audiotool PRIVATE dal_audio_wasapi dal_media)
  add_dependencies(dal_audiotool dal_ffmpeg_runtime)
endfunction()
