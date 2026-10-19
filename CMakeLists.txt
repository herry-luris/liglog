cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(liglog LANGUAGES CXX)

if (NOT DEFINED LIG_LOGGER_LEVEL) 
	set(LIG_LOGGER_LEVEL 0)
endif()
add_compile_definitions(LIG_LOGGER_LEVEL=${LIG_LOGGER_LEVEL})

add_library(liglog STATIC ./src/sink.cpp ./src/formatter.cpp 
	./src/transport.cpp)
target_include_directories(liglog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(liglog PUBLIC cxx_std_20)


include(CTest)
if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()

add_subdirectory(benchmarks)
