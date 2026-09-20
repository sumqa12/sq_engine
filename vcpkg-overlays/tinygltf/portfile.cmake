# tinygltf（ヘッダオンリー）の overlay port（phase14 ③-1）。
#
# なぜ本家 port をそのまま使わないのか:
#   本家は vcpkg_from_github（= GitHub の生成する .tar.gz を落として SHA512 で検証）を使う。
#   ところが GitHub の tarball は動的生成のため、tar の中身が同じでも
#   **再圧縮で gzip のバイト列が変わることがある**。そうなると port に書かれた SHA512 と
#   一致せず、ダウンロードが必ず失敗する（本プロジェクトで 2.9.6 / 3.0.0 の両方で発生）。
#
#   このとき落ちてきた tarball の中身は正当だった（展開した tiny_gltf.h のハッシュが
#   git のタグ v2.9.6 のものと完全に一致することを確認済み）。壊れているのは
#   「配布物の圧縮結果」であって、ソースでも経路でもない。
#
#   → git で取れば解決する。git はコミットSHA（= 内容のハッシュ）で検証するので、
#     圧縮の差異に一切影響されない。SHA512 を自分で書き写す必要も無くなる。
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/syoyo/tinygltf
    REF cfcadfa8d14eb489d97b6324838ae100410edcc7  # タグ v3.0.0 が指すコミット
)

# tiny_gltf.h は既定で同梱の "json.hpp" を見に行くが、vcpkg では nlohmann-json が
# 別パッケージとして入るので、include を差し替える（本家 port と同じ処理）。
vcpkg_replace_string("${SOURCE_PATH}/tiny_gltf.h" "#include \"json.hpp\"" "#include <nlohmann/json.hpp>")

file(INSTALL "${SOURCE_PATH}/tiny_gltf.h" "${SOURCE_PATH}/tiny_gltf_v3.h" "${SOURCE_PATH}/tinygltf_json.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
