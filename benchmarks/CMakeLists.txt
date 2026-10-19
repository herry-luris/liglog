find_package(benchmark REQUIRED)
find_package(Threads REQUIRED)

add_executable(bench_liglog bench_liglog.cpp)
target_include_directories(bench_liglog PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_liglog PRIVATE
    liglog
    benchmark::benchmark
    benchmark::benchmark_main
    Threads::Threads
)
target_compile_features(bench_liglog PRIVATE cxx_std_20)

find_package(spdlog QUIET)

if(spdlog_FOUND)
    add_executable(bench_spdlog bench_spdlog.cpp)
    target_link_libraries(bench_spdlog PRIVATE
        spdlog::spdlog
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
    )
    target_compile_features(bench_spdlog PRIVATE cxx_std_17)
else()
    message(STATUS "spdlog not found, skipping bench_spdlog")
endif()


add_executable(bench_throughput bench_throughput.cpp)
target_include_directories(bench_throughput PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bench_throughput PRIVATE
    liglog
    spdlog::spdlog
    Threads::Threads
)
target_compile_features(bench_throughput PRIVATE cxx_std_20)
