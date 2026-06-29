cmake_minimum_required(VERSION 3.16)

project(TinyHttpServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(tiny_http_core
    src/http_parser.cpp
    src/http_response.cpp
    src/mime_types.cpp
    src/linux_server.cpp
    src/thread_pool.cpp
    src/async_logger.cpp
    src/config_parser.cpp
)

target_include_directories(tiny_http_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(tiny_http_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

add_executable(parser_tests
    tests/parser_tests.cpp
)

add_executable(config_parser_tests
    tests/config_parser_tests.cpp
)

add_executable(tiny_httpd
    src/main.cpp
)

target_link_libraries(parser_tests PRIVATE
    tiny_http_core
)

target_link_libraries(config_parser_tests PRIVATE
    tiny_http_core
)

target_link_libraries(tiny_httpd PRIVATE
    tiny_http_core
)

enable_testing()

add_test(NAME parser_tests COMMAND parser_tests)
add_test(NAME config_parser_tests COMMAND config_parser_tests)