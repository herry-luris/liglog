find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(lig_tests
    test_component.cpp
    test_logger.cpp
    test_backend.cpp
    test_integration.cpp
    test_performance.cpp
)

target_include_directories(lig_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lig_tests PRIVATE
	liglog
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)

target_compile_features(lig_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(lig_tests)
