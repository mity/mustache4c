
include_directories("${PROJECT_SOURCE_DIR}/src")

add_executable(test-spec acutest.h json.h json.c test_spec.c)
target_link_libraries(test-spec mustache)
