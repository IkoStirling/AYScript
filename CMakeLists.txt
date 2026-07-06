cmake_minimum_required(VERSION 3.20)
project(AYScript VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Third-party deps ----
# sol2 3.5.0 (header-only INTERFACE target) + Lua 5.5 (vcpkg)
find_package(sol2 CONFIG REQUIRED)
find_package(Lua REQUIRED)

# ---- Library sources ----
# Globs cover src/logia/*.cpp (S0 Lexer/Parser/AST/Compiler) and
# src/*.cpp (S1 runtime bridge + subsystem).
file(GLOB AYSCRIPT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logia/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(AYScript STATIC ${AYSCRIPT_SOURCES})

target_include_directories(AYScript PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Public link only exposes AYLog — sol2/lua stay PRIVATE so consumers
# don't accidentally include sol.hpp.
target_link_libraries(AYScript
    PUBLIC  AYLog
    PRIVATE sol2::sol2 ${LUA_LIBRARIES}
)

# Enable sol2 type & numeric safety checks.
target_compile_definitions(AYScript PRIVATE
    SOL_ALL_SAFETIES_ON=1
    SOL_SAFE_NUMERICS=1
)

if(MSVC)
    target_compile_options(AYScript PRIVATE /Zc:__cplusplus /Zc:preprocessor)
endif()

enable_testing()
add_subdirectory(unittest)