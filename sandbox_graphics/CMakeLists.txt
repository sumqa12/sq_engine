# 検証用シーン（scenes/）。phase17 ④の前に main.cpp から分離した。
# ★ シーンを足したら、ここに .cpp を足すこと（GLOB にしていないので、足し忘れるとリンクエラーで気付ける）。
add_executable(sandbox_graphics
        main.cpp
        scenes/scene_registry.cpp
        scenes/scene_helpers.cpp
        scenes/content_instancing.cpp
        scenes/content_gltf.cpp
        scenes/content_materials.cpp
        scenes/content_shadows.cpp
        scenes/content_lighting.cpp)
target_link_libraries(sandbox_graphics PRIVATE sq_engine_lib)

# シェーダーをSPIR-Vにコンパイルし、実行ファイルと同じディレクトリの shaders/ に出力する。
# Renderer は "shaders/triangle.vert.spv" のような相対パスを実行時CWD基準で開くため、
# 実行ファイルの隣にコピーしておくことでCWDに依存せず動作する。
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
# ビルドツリー内の固定の場所に一旦コンパイル（OUTPUTはターゲット非依存にする）
set(SHADER_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders_spv)

# phase16 ②-3: コンピュートシェーダ（*.comp）を追加。glslc は拡張子からステージを判定する。
# ★ GLOB は構成時にしか評価されないので、シェーダを足したら CMake を再実行すること。
file(GLOB SHADERS
        ${SHADER_SOURCE_DIR}/*.vert
        ${SHADER_SOURCE_DIR}/*.frag
        ${SHADER_SOURCE_DIR}/*.comp)

# phase17 ⓪-1: シェーダの共通定義（shaders/include/*.glsl）を集める。
file(GLOB SHADER_INCLUDES
        ${SHADER_SOURCE_DIR}/include/*.glsl)
#   ★ 上の SHADERS の GLOB は *.vert / *.frag / *.comp しか拾わないので、
#     include/*.glsl が単独でコンパイルされることはない（拡張子を .glsl にした理由）。
#   ★ GLOB を足したら CMake の再実行が要る。

foreach(source IN LISTS SHADERS)
    get_filename_component(FILENAME ${source} NAME)
    add_custom_command(
            OUTPUT ${SHADER_BUILD_DIR}/${FILENAME}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_BUILD_DIR}
            COMMAND ${glslc_executable}
            -o ${SHADER_BUILD_DIR}/${FILENAME}.spv
            -I ${SHADER_SOURCE_DIR}/include
            ${source}
            DEPENDS ${source} ${SHADER_INCLUDES}
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

# テクスチャ画像を実行ファイルの隣の textures/ にコピーする（Renderer は "textures/<name>.png" を
# 実行時CWD基準で開く）。
add_custom_command(TARGET sandbox_graphics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sandbox_graphics>/textures
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/textures $<TARGET_FILE_DIR:sandbox_graphics>/textures
        COMMENT "Copying textures next to sandbox_graphics.exe"
)

# glTF モデルを実行ファイルの隣の models/ にコピーする（phase14 ③）。
# テクスチャと同じ理由・同じ形。アプリ側は "models/<name>.glb" を実行時CWD基準で開く。
#
# ★ .gltf（JSON形式）を置く場合は、参照している .bin や画像も同じ相対位置に
#   揃っていないと読めない。copy_directory はディレクトリを丸ごと写すので、
#   assets/models の中の構造さえ保っていればそのまま動く。
add_custom_command(TARGET sandbox_graphics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sandbox_graphics>/models
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/models $<TARGET_FILE_DIR:sandbox_graphics>/models
        COMMENT "Copying models next to sandbox_graphics.exe"
)
