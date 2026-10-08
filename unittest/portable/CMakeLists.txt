cmake_minimum_required(VERSION 3.20)
project(AYScriptDeterministicChecks LANGUAGES C CXX)
get_filename_component(SCRIPT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(ENGINE_ROOT "${SCRIPT_ROOT}/../.." ABSOLUTE)
set(MATH_ROOT "${ENGINE_ROOT}/AYFoundation/AYMath")
set(ENTITY_ROOT "${ENGINE_ROOT}/AYRuntime/AYEntity")
include("${MATH_ROOT}/cmake/SoftFloat.cmake")
aymath_add_softfloat(ScriptBackend "${MATH_ROOT}")
add_executable(AYScript_DeterministicChecks main.cpp
    "${SCRIPT_ROOT}/determinism/DeterministicLogia.cpp"
    "${SCRIPT_ROOT}/src/logia/AYLexer.cpp" "${SCRIPT_ROOT}/src/logia/AYParser.cpp"
    "${SCRIPT_ROOT}/src/logia/AYCompilerError.cpp"
    "${ENTITY_ROOT}/src/AYDeterministicSession.cpp"
    "${ENTITY_ROOT}/src/AYDeterministicRollback.cpp"
    "${ENTITY_ROOT}/src/AYDeterministicSessionWire.cpp" "${ENTITY_ROOT}/src/AYDeterministicState.cpp"
    "${MATH_ROOT}/src/DetFloat.cpp" "${MATH_ROOT}/src/DetMath.cpp"
    $<TARGET_OBJECTS:ScriptBackend>)
target_compile_features(AYScript_DeterministicChecks PRIVATE cxx_std_23)
target_include_directories(AYScript_DeterministicChecks PRIVATE "${SCRIPT_ROOT}/include"
    "${ENTITY_ROOT}/include" "${MATH_ROOT}/include" "${MATH_ROOT}/third_party/softfloat/include"
    "${ENGINE_ROOT}/AYFoundation/AYReplay/include")
if(MSVC)
    target_compile_options(AYScript_DeterministicChecks PRIVATE /utf-8 /fp:fast)
else()
    target_compile_options(AYScript_DeterministicChecks PRIVATE -ffast-math)
endif()
enable_testing()
add_test(NAME AYScript_DeterministicChecks COMMAND AYScript_DeterministicChecks)
set_tests_properties(AYScript_DeterministicChecks PROPERTIES TIMEOUT 120)
