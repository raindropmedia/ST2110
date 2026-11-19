add_executable(st2110_tests
    test_main.cpp
    test_frame_rate.cpp
    test_options.cpp
    test_report.cpp
    test_timing_stats.cpp
)
target_link_libraries(st2110_tests PRIVATE st2110_core doctest::doctest)
add_test(NAME st2110_tests COMMAND st2110_tests)
