add_executable(st2110_tests
    test_main.cpp
    test_frame_rate.cpp
    test_audio.cpp
    test_discovery.cpp
    test_histogram.cpp
    test_monitor.cpp
    test_options.cpp
    test_preview.cpp
    test_ptp.cpp
    test_sdp.cpp
    test_system_info.cpp
    test_timing_stats.cpp
    test_test_sender.cpp
    test_test_signal.cpp
    test_video_timing.cpp
    ../src/demo/DemoSystemProbe.cpp
)
target_link_libraries(st2110_tests PRIVATE st2110_core doctest::doctest)
add_test(NAME st2110_tests COMMAND st2110_tests)
