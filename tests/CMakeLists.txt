find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ecs_tests ecs_tests.cpp)
target_link_libraries(ecs_tests PRIVATE sq_engine_lib Catch2::Catch2WithMain)

include(CTest)

# catch_discover_tests()はビルド時にecs_tests.exeを実行し、テスト一覧のJSON出力を
# CMakeのstring(JSON)でパースするが、その経路でWindowsのコンソールコードページ変換が
# 入り、日本語のTEST_CASE名がUTF-8として正しく扱われず文字化け/パース失敗する。
# そのため個別テスト発見はせず、実行ファイル全体を1つのCTestとして登録する。
add_test(NAME ecs_tests COMMAND ecs_tests)
