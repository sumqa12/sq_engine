add_executable(sandbox_graphics main.cpp)
target_link_libraries(sandbox_graphics PRIVATE sq_engine_lib)

# シェーダーをSPIR-Vにコンパイルし、実行ファイルと同じディレクトリの shaders/ に出力する。
# Renderer は "shaders/triangle.vert.spv" のような相対パスを実行時CWD基準で開くため、
# 実行ファイルの隣にコピーしておくことでCWDに依存せず動作する。
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
# ビルドツリー内の固定の場所に一旦コンパイル（OUTPUTはターゲット非依存にする）
set(SHADER_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders_spv)

file(GLOB SHADERS
        ${SHADER_SOURCE_DIR}/*.vert
        ${SHADER_SOURCE_DIR}/*.frag)

foreach(source IN LISTS SHADERS)
    get_filename_component(FILENAME ${source} NAME)
    add_custom_command(
            OUTPUT ${SHADER_BUILD_DIR}/${FILENAME}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_BUILD_DIR}
            COMMAND ${glslc_executable}
            -o ${SHADER_BUILD_DIR}/${FILENAME}.spv
            ${source}
            DEPENDS ${source}
            COMMENT "Compiling ${FILENAME}"
    )
    list(APPEND SPV_SHADERS ${SHADER_BUILD_DIR}/${FILENAME}.spv)
endforeach()

add_custom_target(sandbox_graphics_shaders DEPENDS ${SPV_SHADERS})
add_dependencies(sandbox_graphics sandbox_graphics_shaders)

# exe の隣（実際の出力先）に丸ごとコピー。ここなら $<TARGET_FILE_DIR> が使える。
add_custom_command(TARGET sandbox_graphics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sandbox_graphics>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_BUILD_DIR} $<TARGET_FILE_DIR:sandbox_graphics>/shaders
        COMMENT "Copying shaders next to sandbox_graphics.exe"
)
