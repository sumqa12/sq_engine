#include "sq/assets/gltf_loader.hpp"

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>
#include <numeric>

#include <glm/gtc/type_ptr.hpp>   // glm::make_mat4（matrix 形式のノード変換。③-4 手順5）
#include <__msvc_ranges_to.hpp>
#include <spdlog/spdlog.h>

// ★ stb の二重定義に注意（phase14 ③-1）。
//   texture.cpp が既に STB_IMAGE_IMPLEMENTATION を定義しているため、
//   tinygltf に stb_image の実装を展開させると多重定義でリンクエラーになる。
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#include <stb_image.h>
#define TINYGLTF_IMPLEMENTATION      // ★ 実体化はこの翻訳単位でのみ
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

namespace sq::assets {

// ============================================================================
// 内部ヘルパ（phase14 ③-4）
//
// すべて匿名名前空間に置く。tinygltf の型が引数・戻り値に出てくるので、
// 公開ヘッダ（gltf_loader.hpp）には**絶対に載せない**。載せると
// tiny_gltf.h を include するすべての翻訳単位に nlohmann-json が波及し、
// コンパイル時間が跳ね上がるうえ、ライブラリの差し替えもできなくなる。
// ============================================================================
namespace {

// -- 1. アクセサ読み出し（★ ここから書くこと）--------------------------------

// アクセサ accessor_index を T の配列として読む。
//
// T は glm::vec2 / glm::vec3 / glm::vec4 を想定（= float ベースの頂点属性）。
// インデックスは componentType が3種類に分かれるので別関数（read_indices）にしてある。
//
// ★ **この関数がこのフェーズの心臓部**。POSITION / NORMAL / TEXCOORD_0 のすべてが
//   ここを通るので、ここが正しければ大半は動く。逆に場当たりで書くと
//   「一部のモデルだけ壊れる」という最も追いにくい症状になる。
template <typename T>
[[nodiscard]] std::vector<T> read_accessor(const tinygltf::Model& model, int accessor_index) {
    //   1. 属性が無いプリミティブなら空を返す
    if (accessor_index < 0) {
        return {};
    }

    //   2. アクセサ・バッファビュー・バッファを取得
    const auto& accessor    = model.accessors[accessor_index];
    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    const auto& buffer      = model.buffers[buffer_view.buffer];

    //   3. 要素の先頭アドレス:
    const unsigned char* base = buffer.data.data()
                              + buffer_view.byteOffset + accessor.byteOffset;
    //      ★ **オフセットが2段ある**（bufferView と accessor）。片方だけ足すと全部ずれる。

    //   4. ★ stride を必ず見る:
    //
    //      glTF のバッファには2通りの並び方があり、stride はその差を吸収する値。
    //
    //      (a) 詰まっている（属性ごとに別の bufferView。byteStride は未指定＝0）
    //          ┌────────┬────────┬────────┬────────┐
    //          │ POS 0  │ POS 1  │ POS 2  │ POS 3  │   各12バイト
    //          └────────┴────────┴────────┴────────┘
    //           0        12       24       36            → stride = 12
    //
    //      (b) インターリーブ（1本の bufferView に複数属性が交互に入る。byteStride = 32）
    //          ┌────────┬────────┬─────┬────────┬────────┬─────┬────────┐
    //          │ POS 0  │ NRM 0  │ UV0 │ POS 1  │ NRM 1  │ UV1 │ POS 2  │
    //          └────────┴────────┴─────┴────────┴────────┴─────┴────────┘
    //           0        12       24    32       44       56    64
    //           ↑                       ↑                       ↑
    //           POSITION が読むべき位置（byteOffset=0, stride=32）
    //           NORMAL は byteOffset=12, stride=32 で 12 / 44 / 76 を読む
    //
    //      ★ (b) で sizeof(T)=12 ずつ進めると 0 → 12 → 24 を読むことになり、
    //        「1番目の頂点位置」として法線が、「2番目」として UV が入ってくる。
    //        これが「ジオメトリが崩壊する」の中身。**落ちない**ので、
    //        画面にぐちゃぐちゃな三角形が出るだけで原因にたどり着けない。
    //
    //      ByteStride() は (a) で要素サイズを、(b) で byteStride を返してくれるので、
    //      自前で if (byteStride == 0) を書かずに両方を同じループで扱える。
    //      ★ 戻り値は int で、不正な設定のとき **-1** を返す（tiny_gltf.h の実装を参照）。
    //        size_t へキャストする前に必ず弾くこと（-1 が巨大な値に化ける）。
    const int stride_i = accessor.ByteStride(buffer_view);
    if (stride_i <= 0) {
        spdlog::warn("read_accessor : byteStride が不正です。");
        return {};
    }
    const auto stride = static_cast<std::size_t>(stride_i);

    //   5. ★ 型の検証（★ 必ず下の memcpy ループより**前**に置くこと）:
    //      componentType が FLOAT か、成分数（VEC2 / VEC3 / VEC4）が T と一致するかを確かめる。
    //      （正規化整数の頂点属性を使うモデルもあるが、このフェーズでは対応しない）
    //
    //      ★ 後ろに置くと意味が無い。下の memcpy は sizeof(T) バイトを読むので、
    //        VEC3 のアクセサを T = glm::vec4 で読もうとすると、
    //        **検証にたどり着く前に**1要素ごとに4バイトずつはみ出して読んでいる。
    //
    //      ★ accessor.type の生の値は VEC2/VEC3/VEC4 に限れば 2/3/4 なので、
    //        そのまま成分数として比較しても通る。ここで GetNumComponentsInType() を
    //        通しているのは、その一致が **VEC 系だけの偶然**だから
    //        （MAT2 = 32+2、SCALAR = 64+1）。SCALAR を読む関数へこの式をコピーしても
    //        壊れないのが、この書き方の利点。
    //      ★ T::length() を使うと sizeof(glm::vec3) == 12 という GLM の既定設定への
    //        依存も消える（GLM_FORCE_ALIGNED_GENTYPES が入ると 16 になる）。
    constexpr int kComponents = T::length();   // glm の静的 constexpr。vec3 なら 3
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        tinygltf::GetNumComponentsInType(static_cast<std::uint32_t>(accessor.type)) != kComponents) {
        spdlog::warn("read_accessor : アクセサの型が不正です。");
        return {};
    }

    //      範囲チェック。壊れた glTF や切り詰められた .glb で buffer.data の外を読まないため。
    if (accessor.count == 0) {
        spdlog::warn("read_indices : アクセサがありません。");
        return {};
    }
    const std::size_t need = stride * (accessor.count - 1) + sizeof(float) * kComponents;
    if (accessor.byteOffset + need > buffer_view.byteLength) {
        spdlog::warn("read_accessor : アクセサが bufferView をはみ出しています。");
        return {};
    }

    //   6. i = 0..accessor.count-1 について base + stride * i から読んで push_back する
    //
    //      ★ *reinterpret_cast<const T*>(base + stride * i) ではなく memcpy を使う理由:
    //        - アライメント: glTF が保証するのは「コンポーネント型のサイズ境界」（float なら
    //          4バイト）までで、alignof(T) は保証されない。既定の glm::vec3 は alignof が 4
    //          なので実際には通るが、GLM_FORCE_ALIGNED_GENTYPES が入ると未アライメントアクセスになる
    //        - strict aliasing: 生バイト列を T* として読むのは規格上は未定義動作。
    //          memcpy が標準的に許された経路
    //        ★ 実行コストの心配は要らない。12バイトの memcpy は最適化が効けば
    //          そのままロード命令1〜2個になる。

    std::vector<T> result;
    result.reserve(accessor.count); // count は「要素数」であってバイト数ではない
    for (std::size_t i = 0; i < accessor.count; ++i) {
        T value{};
        std::memcpy(&value, base + stride * i, sizeof(T));
        result.push_back(value);
    }

    return result;
}

// インデックスアクセサを uint32 の配列として読む。
//
// ★ 頂点属性と分けてあるのは、componentType が
//   UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT の3通りに分かれるため。
//   read_accessor<T> のように「T で読む」形にできない（ファイル側の型が可変）。
//
[[nodiscard]] std::vector<std::uint32_t> read_indices(const tinygltf::Model& model,
                                                      int accessor_index) {
    //   1. read_accessor と同じ要領で base と stride を求める
    const auto& accessor    = model.accessors[accessor_index];
    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    const auto& buffer      = model.buffers[buffer_view.buffer];

    const unsigned char* base = buffer.data.data()
                              + buffer_view.byteOffset + accessor.byteOffset;

    const int stride_i = accessor.ByteStride(buffer_view);
    if (stride_i <= 0) {
        spdlog::warn("read_indices : byteStride が不正です。");
        return {};
    }
    const auto stride = static_cast<std::size_t>(stride_i);

    //   型チェック
    const auto elem = static_cast<std::size_t>(
    tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(accessor.componentType)));
    if (elem == 0 || accessor.type != TINYGLTF_TYPE_SCALAR) {
        spdlog::warn("read_indices : インデックスの形が不正です。 accessor.type: {}", accessor.type);
        return {};
    }

    //   アクセサの存在チェック
    if (accessor.count == 0) {
        spdlog::warn("read_indices : インデックスがありません。");
        return {};
    }

    //   範囲チェック。壊れた glTF や切り詰められた .glb で buffer.data の外を読まないため。
    const std::size_t need = stride * (accessor.count - 1) + elem;
    if (accessor.byteOffset + need > buffer_view.byteLength) {
        spdlog::warn("read_indices : インデックスが bufferView をはみ出しています。");
        return {};
    }

    //   2. accessor.componentType で分岐し、1要素ずつ uint32 へ広げて詰める
    //        TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE  → std::uint8_t
    //        TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT → std::uint16_t
    //        TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT   → std::uint32_t
    //      それ以外は spdlog::warn を出して空配列を返す
    std::vector<std::uint32_t> result;
    result.reserve(accessor.count);

    auto read_as = [&]<typename Src>() {
        for (std::size_t i = 0; i < accessor.count; ++i) {
            Src src;
            std::memcpy(&src, base + stride * i, sizeof(src));
            result.push_back(src);          // Src → uint32 のゼロ拡張
        }
    };

    switch (accessor.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  read_as.operator()<std::uint8_t>();  break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: read_as.operator()<std::uint16_t>(); break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   read_as.operator()<std::uint32_t>(); break;
        default:
            spdlog::warn("read_indices : インデックスの componentType が不正です。");
            return {};
    }


    //   3. ★ ここでは常に uint32 で返し、「uint16 に収まるか」の判断は呼び出し側
    //      （load_primitive）に任せる。そうしないと戻り値の型が2つに割れて扱いにくい。
    return result;
}

// -- 2. 画像・テクスチャ ------------------------------------------------------

// tinygltf::Image のピクセル列を RGBA8 に揃える。
//
// ★ tinygltf は元画像の形をそのまま渡してくる。Texture 側は RGBA8 前提なので、
//   ここで吸収しないとバイト数が合わないまま転送して画像がずれる・踏み越える。
[[nodiscard]] bool to_rgba8(const tinygltf::Image& image, std::vector<unsigned char>& out) {
    //   1. image.bits != 8 なら false を返す（16bit PNG。このフェーズでは非対応）
    if (image.bits != 8) {
        return false;
    }
    //   2. image.component == 4 なら image.image をそのまま out へコピーして true
    std::size_t image_len = image.image.size();
    if (image.component == 4) {
        out.resize(image_len);
        std::memcpy(out.data(), image.image.data(), image_len);
        return true;
    }
    //   3. image.component == 3 なら RGB → RGBA へ展開（アルファは 255）して true
    if (image.component == 3) {
        out.resize(image_len / 3 * 4);
        for (std::size_t i = 0, o = 0; i + 2 < image_len; i += 3) {
            out[o++] = image.image[i];
            out[o++] = image.image[i + 1];
            out[o++] = image.image[i + 2];
            out[o++] = 255;
        }
        return true;
    }

    //   4. それ以外（1 = グレースケール, 2 = グレー+アルファ）は
    //      当面 false を返してよい。必要になったら展開を足す
    //   ★ false のときは呼び出し側が spdlog::warn を出して既定テクスチャに落とす。
    //     「黙って変な絵を出す」より「警告付きで既定テクスチャ」の方が原因に気付ける。
    return false;
}

// model.images をすべて登録し、「glTF内の image 添字 → TextureId」の対応表を返す。
[[nodiscard]] std::vector<scene::TextureId> load_images(const tinygltf::Model& model,
                                                        graphics::TextureRegistry& textures) {

    std::vector<scene::TextureId> result;

    //   1. サイズの確保
    result.reserve(model.images.size());

    //   2. 各 image について to_rgba8 → TextureRegistry::load_from_pixels
    for (auto& image : model.images) {
        std::vector<unsigned char> pixels;
        scene::TextureId id;
        if (to_rgba8(image, pixels)) {
            id = textures.load_from_pixels(pixels.data(), static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height));
        } else {
            // 失敗した場合、規定テクスチャを入れる（入れないとずれる）
            spdlog::warn("load_images : rgba8への変換に失敗しました。");
            id = textures.default_texture();
        }
        result.push_back(id);
    }

    return result;
}

// glTF の **texture** 添字から TextureId を引く。
//
// ★ マテリアルが参照するのは image ではなく texture。
//   texture は「image + sampler」の組なので、model.textures[i].source で
//   image 添字へ1段たどる必要がある。ここを飛ばすと別の絵が貼られる。
[[nodiscard]] scene::TextureId resolve_texture(const tinygltf::Model& model,
                                               const std::vector<scene::TextureId>& image_ids,
                                               int texture_index,
                                               const graphics::TextureRegistry& textures) {

    // 手順:
    //   1. texture_index < 0 は「このマテリアルはテクスチャを持たない」。
    //      ★ 異常ではないので、**中立な白**を返す（phase14 ③）。
    //        glTF の仕様では baseColorTexture が無いマテリアルの基本色は
    //        baseColorFactor そのもの。白を乗算すれば恒等変換になり仕様と一致する。
    //        ここで市松模様の default_texture() を返すと、無地のモデル（Box.glb など）に
    //        模様が掛かってしまい、「テクスチャ無し」と「読み込み失敗」も区別できなくなる。
    if (texture_index < 0) {
        return textures.white_texture();
    }

    //   2. source < 0 または image_ids の範囲外 → **こちらは異常**なので既定テクスチャ（市松模様）。
    //      texture は存在すると書いてあるのに実体へたどれない＝ファイルが壊れている。
    const int source = model.textures[texture_index].source;
    if (source < 0 || source >= static_cast<int>(image_ids.size())) {
        spdlog::warn("resolve_texture : texture {} の source が不正です。", texture_index);
        return textures.default_texture();
    }

    //   3. return image_ids[source];
    //   ★ sampler（フィルタ・ラップモード）はこのフェーズでは無視する。
    //     本エンジンは全テクスチャで1つの VkSampler を共有している（phase12）。
    return image_ids[source];
}

// -- 3. マテリアル ------------------------------------------------------------

// glTF のマテリアル1件を登録した結果。
// ★ transparent が MaterialId と別に要るのは、これが CPU 側の描画パス振り分け用で
//   MaterialData（GPUレイアウト）に入らないため（phase14 ①-4）。
struct LoadedMaterial {
    scene::MaterialId id;
    bool transparent = false;
};

// model.materials をすべて登録し、「glTF内の material 添字 → 登録結果」の対応表を返す。
[[nodiscard]] std::vector<LoadedMaterial> load_materials(const tinygltf::Model& model,
                                                         const std::vector<scene::TextureId>& image_ids,
                                                         graphics::TextureRegistry& textures,
                                                         graphics::MaterialRegistry& materials) {

    // 対応づけ:
    //   pbrMetallicRoughness.baseColorFactor        → base_color（double[4] なので float へ narrowing）
    //   pbrMetallicRoughness.baseColorTexture.index → resolve_texture して add の第2引数へ
    //   pbrMetallicRoughness.metallicFactor         → metallic
    //   pbrMetallicRoughness.roughnessFactor        → roughness
    //   emissiveFactor                              → emissive（vec3 なので w は 0 のまま）
    //   alphaCutoff                                 → alpha_cutoff
    //   alphaMode == "BLEND"                        → LoadedMaterial::transparent
    //
    // ★ alphaMode は "OPAQUE" / "MASK" / "BLEND" の3値。"MASK" は alpha_cutoff を使う
    //   カットアウトで、半透明パスには送らない（不透明のまま frag で discard する）。
    //   discard 自体は phase15 の課題なので、ここでは値を運ぶだけでよい。
    // ★ ★ 「マテリアルを1つも持たない glTF」がある。その場合この配列は空になり、
    //   primitive.material が -1 になる。呼び出し側で既定マテリアルに落とすこと。
    std::vector<LoadedMaterial> result;
    result.reserve(model.materials.size());
    for (auto& material : model.materials) {
        glm::vec4 base_color(
            static_cast<float>(material.pbrMetallicRoughness.baseColorFactor[0]),
            static_cast<float>(material.pbrMetallicRoughness.baseColorFactor[1]),
            static_cast<float>(material.pbrMetallicRoughness.baseColorFactor[2]),
            static_cast<float>(material.pbrMetallicRoughness.baseColorFactor[3])
        );
        glm::vec4 emissive(
            static_cast<float>(material.emissiveFactor[0]),
            static_cast<float>(material.emissiveFactor[1]),
            static_cast<float>(material.emissiveFactor[2]),
            0
        );
        graphics::MaterialData material_data = {
            .base_color = base_color,
            .emissive =  emissive,
            .metallic = static_cast<float>(material.pbrMetallicRoughness.metallicFactor),
            .roughness = static_cast<float>(material.pbrMetallicRoughness.roughnessFactor),
            .alpha_cutoff = static_cast<float>(material.alphaCutoff),
        };

        spdlog::info("base_color: {}, {}, {}, {}", base_color.r, base_color.g, base_color.b, base_color.a);

        scene::TextureId texture = resolve_texture(model, image_ids,
            material.pbrMetallicRoughness.baseColorTexture.index, textures);

        LoadedMaterial loaded_material = {
            .id = materials.add(material_data, texture),
            .transparent = material.alphaMode == "BLEND"
        };
        result.push_back(loaded_material);
    }

    return result;
}

// -- 4. メッシュ --------------------------------------------------------------

// プリミティブ1つを MeshRegistry へ登録する。読み飛ばす場合は std::nullopt を返す。
[[nodiscard]] std::optional<scene::MeshId> load_primitive(const tinygltf::Model& model,
                                                          const tinygltf::Primitive& primitive,
                                                          graphics::MeshRegistry& meshes) {

    //   1. ★ primitive.mode != TINYGLTF_MODE_TRIANGLES なら
    //      spdlog::warn を出して std::nullopt（STRIP / FAN / POINTS はこのフェーズでは非対応）
    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
        spdlog::warn("load_primitive : プリミティブのモードが不正です。");
        return std::nullopt;
    }

    //   2. 属性を読む。find で存在を確かめてから read_accessor へ:
    //        POSITION   … 必須。無ければ warn + nullopt
    //        NORMAL     … 無ければ {0,1,0} で埋める（phase15 のライティングまで見た目に出ない）
    //        TEXCOORD_0 … 無ければ {0,0} で埋める
    //      ★ 3つの配列は**長さが揃っている前提**。揃っていなければ壊れた glTF なので
    //        warn + nullopt にする（短い方に合わせて読むと静かに壊れる）。
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> tex_coords;
    if (auto it = primitive.attributes.find("POSITION"); it == primitive.attributes.end()) {
        spdlog::warn("load_primitive : プリミティブにPOSITION属性がありません。");
        return std::nullopt;
    } else {
        positions = read_accessor<glm::vec3>(model, it->second);
    }

    if (auto it = primitive.attributes.find("NORMAL"); it == primitive.attributes.end()) {
        normals.reserve(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            normals.emplace_back(0, 1, 0);
        }
    } else {
        normals = read_accessor<glm::vec3>(model, it->second);
        if (positions.size() != normals.size()) {
            spdlog::warn("load_primitive : プリミティブのPOSITION属性とNORMAL属性の要素数が等しくありません。");
            return std::nullopt;
        }
    }

    if (auto it = primitive.attributes.find("TEXCOORD_0"); it == primitive.attributes.end()) {
        tex_coords.reserve(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            tex_coords.emplace_back(0, 0);
        }
    } else {
        tex_coords = read_accessor<glm::vec2>(model, it->second);
        if (positions.size() != tex_coords.size()) {
            spdlog::warn("load_primitive : プリミティブのPOSITION属性とTEXCOORD_0属性の要素数が等しくありません。");
            return std::nullopt;
        }
    }


    //   3. std::vector<Vertex> を組む（position / normal / uv を i 番目ずつ詰める）
    std::vector<graphics::Vertex> vertices;
    vertices.reserve(positions.size());
    for (int i = 0; i < positions.size(); ++i) {
        graphics::Vertex vertex = {
            .position = positions[i],
            .normal = normals[i],
            .uv = tex_coords[i],
        };
        vertices.emplace_back(vertex);
    }

    //   4. read_indices でインデックスを読む
    //      ★ インデックスアクセサが無い（primitive.indices < 0）モデルがある。
    //        その場合は 0,1,2,... を生成する（非インデックス描画の代わり）。
    std::vector<uint32_t> indices;
    if (primitive.indices < 0) {
        indices.resize(positions.size());
        std::iota(indices.begin(), indices.end(), 0u);
    } else {
        indices = read_indices(model, primitive.indices);
    }

    //   5. 頂点数が 65536 を超えるか、インデックスの最大値が 65535 を超えるなら
    //      uint32 版の MeshRegistry::add へ、収まるなら uint16 へ落として uint16 版へ渡す
    scene::MeshId mesh_id;
    if (vertices.size() > 65536) {
        mesh_id = meshes.add(vertices, indices);
    } else {
        std::vector<uint16_t> dst;
        dst.reserve(indices.size());
        std::ranges::copy(
            indices | std::ranges::views::transform([](uint32_t x) {
                return static_cast<uint16_t>(x);
            }),
            std::back_inserter(dst)
        );
        mesh_id = meshes.add(vertices, dst);
    }
    //   6. ★ 巻き順は**触らない**（D-6）。パイプラインを CCW に統一し、
    //      組み込みジオメトリ側を反転させる方針にしたので、読んだままの順序で登録する。

    return mesh_id;
}

// -- 5. ノード ----------------------------------------------------------------

// ノードのローカル変換を TRS として取り出す。
//
// glTF のノードは「TRS 形式」と「matrix 形式」のどちらかで変換を持つ（両方は持たない）。
// LoadedModel::Node は TRS で持つので、matrix のときは分解する。
void decode_node_transform(const tinygltf::Node& node,
                           glm::vec3& position, glm::quat& rotation, glm::vec3& scale) {
    if (node.matrix.size() == 16) {
        //   1. node.matrix.size() == 16 なら matrix 形式
        const auto m = glm::mat4(glm::make_mat4(node.matrix.data()));

        position = glm::vec3(m[3]);                    // 4列目がそのまま平行移動

        glm::mat3 basis(m);                            // 左上3×3を取り出す（= R*S）
        scale = { glm::length(basis[0]),
                  glm::length(basis[1]),
                  glm::length(basis[2]) };

        // 0除算対策。スケール0の軸があると NaN が伝播して、モデルが丸ごと消える
        constexpr float kEps = 1e-8f;
        basis[0] /= std::max(scale.x, kEps);
        basis[1] /= std::max(scale.y, kEps);
        basis[2] /= std::max(scale.z, kEps);

        if (glm::determinant(basis) < 0.0f) {
            scale.x = -scale.x;
            basis[0] = -basis[0];   // 1軸だけ反転を吸収してから quat_cast へ
        }

        rotation = glm::quat_cast(basis);              // ★ 正規化済みの行列を渡すこと
    } else {
        //   2. そうでなければ TRS 形式。それぞれ**無い場合がある**ので既定値を使う:
        //        translation … 無ければ {0,0,0}
        //        rotation    … 無ければ単位クォータニオン
        //                      ★ glTF は (x, y, z, w) の順。glm::quat のコンストラクタは
        //                        (w, x, y, z) の順。**入れ替えないと回転が化ける**
        //        scale       … 無ければ {1,1,1}
        if (node.translation.empty()) {
            position = {0, 0, 0};
        } else {
            position = glm::make_vec3(node.translation.data());
        }
        if (node.rotation.empty()) {
            rotation = {1, 0, 0, 0};
        } else {
            glm::vec4 gltf_rot = glm::make_vec4(node.rotation.data());
            rotation = { gltf_rot.w, gltf_rot.x, gltf_rot.y, gltf_rot.z };
        }
        if (node.scale.empty()) {
            scale = {1, 1, 1};
        } else {
            scale = glm::make_vec3(node.scale.data());
        }
    }
}

}  // namespace

LoadedModel load_gltf(const std::string& path,
                      graphics::MeshRegistry& meshes,
                      graphics::TextureRegistry& textures,
                      graphics::MaterialRegistry& materials) {

    LoadedModel result;
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    // (1) 読み込み
    const bool is_glb = std::filesystem::path(path).extension() == ".glb";
    const bool ok = is_glb
        ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
        : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty()) {
        spdlog::warn("load_gltf : {}", warn);   // ★ 成功時にも出る
    }
    if (!ok || !err.empty()) {
        spdlog::error("load_gltf : {}", err);
        return result;
    }

    // (2)(3) アセットを登録して対応表を作る
    const std::vector<scene::TextureId> image_ids = load_images(model, textures);
    const std::vector<LoadedMaterial> material_table =
        load_materials(model, image_ids, textures, materials);

    // (4) メッシュ。glTF の mesh 1件が複数プリミティブを持つので、
    //     「mesh 添字 → プリミティブの並び」の二次元の対応表になる。
    //     ★ ノードは mesh を指すので、ノード走査の前にここを済ませておく。
    std::vector<std::vector<std::pair<scene::MeshId, scene::MaterialId>>> mesh_table;
    std::vector<std::vector<bool>> mesh_transparent;
    for (auto& mesh : model.meshes) {
        auto& pairs = mesh_table.emplace_back();
        auto& transparent = mesh_transparent.emplace_back();
        for (auto& primitive : mesh.primitives) {
            if (std::optional<scene::MeshId> mesh_id = load_primitive(model, primitive, meshes)) {
                const bool has_material = primitive.material >= 0
                    && primitive.material < static_cast<int>(material_table.size());
                const scene::MaterialId material_id = has_material
                    ? material_table[primitive.material].id
                    : materials.default_material();
                pairs.emplace_back(mesh_id.value(), material_id);
                transparent.emplace_back(has_material && material_table[primitive.material].transparent);
            }
        }
    }

    // (5) ノード
    //     a. 各ノードについて decode_node_transform で TRS を埋める
    result.nodes.resize(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        auto& r_node = result.nodes[i];
        decode_node_transform(model.nodes[i], r_node.position, r_node.rotation, r_node.scale);
    }

    //     b. ★ 親リンクを逆向きに埋める: glTF は node.children に**子の添字**を持つので、
    //        全ノードを走査して result.nodes[child].parent = i を立てる。
    //        （親を持たなかったノードは kNoParent のまま = ルート）
    for (int i = 0; i < model.nodes.size(); i++) {
        auto& node = model.nodes[i];
        for (auto child : node.children) {
            result.nodes[child].parent = i;
        }

        //     c. node.mesh >= 0 なら mesh_table[node.mesh] を primitives へ、
        //        mesh_transparent[node.mesh] を transparent へコピーする
        //        ★ primitives と transparent は**必ず同じ長さ**にすること。
        //          spawn_model が同じ添字で両方を引く（片方だけ短いと範囲外アクセス）。
        auto& result_node = result.nodes[i];
        if (node.mesh >= 0) {
            result_node.primitives = mesh_table[node.mesh];
            result_node.transparent = mesh_transparent[node.mesh];
        }
        //   ★ model.scenes[model.defaultScene].nodes がシーンのルート一覧だが、
        //     全ノードを一律に result.nodes へ入れる方が単純で、孤立ノードが混じっても
        //     「親を持たない空のノード」になるだけで害が無い。このフェーズではそれでよい。
    }

    // (6) 巻き順は触らない（D-6。読んだままの順序で登録済み）
    return result;
}

}  // namespace sq::assets
