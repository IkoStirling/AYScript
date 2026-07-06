set(TEST_NAME AYScript_Test)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})

target_link_libraries(${TEST_NAME} PRIVATE
    AYScript
    AYTest
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
