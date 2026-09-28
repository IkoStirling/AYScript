set(TEST_NAME AYScript_Test)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# S3.9: also compile the shared CliCompile.cpp so the CLI tests can
# call compileFromCli() directly without forking the executable.
list(APPEND TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cli/CliCompile.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})

# S3.9: bake the ays-logia output path into the test TU so e2e CLI
# tests find the binary regardless of VS debugger cwd. file(GENERATE)
# supports $<TARGET_FILE:...> for multi-config generators (x64-Debug).
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CliTestPaths.h"
    CONTENT "#pragma once\n#define AYSCRIPT_AYS_LOGIA_EXE_PATH \"$<TARGET_FILE:ays-logia>\"\n"
)

# Ensure ays-logia is built before the unittest links/runs.
add_dependencies(${TEST_NAME} ays-logia)

# S3.9: CLI tests include the shared compile entry point from cli/.
target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cli
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(${TEST_NAME} PRIVATE
    AYSCRIPT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(${TEST_NAME} PRIVATE
    AYScript
    AYTest
    AYEntity
    AYReflect
    AYIO
    # INT-02 (2026-07-15): Test_LogiaDeviceInput.cpp instantiates a
    # DeviceManager + DeviceInputProvider and feeds KeyboardDevice
    # events through InputMapping. Link AYDevice (for DeviceManager,
    # KeyboardDevice, InputMapping) + AYDeviceSubSystem (for
    # DeviceInputProvider �?lives in SubSystem target because core
    # AYDevice is AYScript-dep-free by design).
    AYDevice
    AYDeviceSubSystem
)

ay_add_test_tiers(${TEST_NAME} PREFIX script TIMEOUT 120
    INTEGRATION_SUITES LogiaCliTests LogiaHotReloadWatcherTests LogiaScriptSubSystemPlayTests)
