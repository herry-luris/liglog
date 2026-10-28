cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(liglog LANGUAGES CXX)

option(LIGLOG_BUILD_TEST "build liglog tests" ON)
option(LIGLOG_BUILD_BENCHMARKS "build liglog tests" ON)

if (NOT DEFINED LIG_LOGGER_LEVEL)
	set(LIG_LOGGER_LEVEL 0)
endif()
add_compile_definitions(LIG_LOGGER_LEVEL=${LIG_LOGGER_LEVEL})

add_library(liglog STATIC
	${CMAKE_CURRENT_SOURCE_DIR}/src/sink.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/formatter.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/transport.cpp)
target_include_directories(liglog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(liglog PUBLIC cxx_std_20)


if(LIGLOG_BUILD_TEST)
	include(CTest)
	enable_testing()
	add_subdirectory(tests)
endif()

if (LIGLOG_BUILD_BENCHMARKS)
	add_subdirectory(benchmarks)
endif()
