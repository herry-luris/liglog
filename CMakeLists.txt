cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(liglog LANGUAGES CXX)

add_library(liglog STATIC ./src/backend.cpp ./src/component.cpp)
target_include_directories(liglog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(liglog PUBLIC cxx_std_20)


include(CTest)
if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()

add_subdirectory(bench)
