cmake_minimum_required(VERSION 3.21)
project(sq_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # ソース/実行文字セットをUTF-8に固定し、日本語リテラルの文字化けを防ぐ
    add_compile_options(/utf-8)
endif ()

enable_testing()

add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(sandbox_graphics)
add_subdirectory(tests)
