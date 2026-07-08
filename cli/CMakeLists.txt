# cli/CMakeLists.txt - ays-logia CLI executable (S3.9).
#
# Tiny static executable that drives the Logia compile pipeline from
# the command line. Used by CI (compile-check .logia fixtures) and by
# the editor's "build to Lua" action.

add_executable(ays-logia
    CliCompile.cpp
    main.cpp
)

target_include_directories(ays-logia PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The CLI uses only the Logia front-end + codegen pipeline; it does
# NOT need sol2, Lua, or AYEntity. Linking just AYScript keeps the
# binary small and removes the runtime engine as a build dep.
target_link_libraries(ays-logia PRIVATE AYScript)