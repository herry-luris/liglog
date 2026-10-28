function(liglog_add_benchmark name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE liglog)
    target_compile_options(${name} PRIVATE -O2 -DNDEBUG)
endfunction()

liglog_add_benchmark(bench_liglog)

# spdlog benchmark: only built when the spdlog target is available.
find_package(spdlog CONFIG REQUIRED)
if(TARGET spdlog::spdlog)
    add_executable(bench_spdlog ${CMAKE_CURRENT_SOURCE_DIR}/bench_spdlog.cpp)
    target_link_libraries(bench_spdlog PRIVATE spdlog::spdlog)
    target_compile_options(bench_spdlog PRIVATE -O2 -DNDEBUG)
endif()
