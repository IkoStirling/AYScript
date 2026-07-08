set(TEST_NAME AYScript_Test)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# S3.9: also compile the shared CliCompile.cpp so the CLI tests can
# call compileFromCli() directly without forking the executable.
list(APPEND TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cli/CliCompile.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})

# S3.9: CLI tests include the shared compile entry point from cli/.
target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cli
)

target_link_libraries(${TEST_NAME} PRIVATE
    AYScript
    AYTest
    AYEntity
    AYReflect
    AYIO
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})