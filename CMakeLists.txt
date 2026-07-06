cmake_minimum_required(VERSION 3.20)
project(AYScript VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB AYSCRIPT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logia/*.cpp"
)

add_library(AYScript STATIC ${AYSCRIPT_SOURCES})

target_include_directories(AYScript PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(AYScript PRIVATE /Zc:__cplusplus /Zc:preprocessor)
endif()

enable_testing()
add_subdirectory(unittest)
