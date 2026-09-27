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

// -- 1.5. 接空間・法線の生成（phase15 ③-4 / ③-5 (E)）-------------------------

// 面法線を頂点へ累積して正規化する（NORMAL が無い glTF 用。phase15 ③-5 (E)）。
//
// ★ 仕様上は「NORMAL が無ければフラットシェーディング」だが、頂点を共有したまま
//   平均化すると厳密なフラットにはならない（このフェーズはそれでよいとする）。
[[nodiscard]] std::vector<glm::vec3> compute_flat_normals(const std::vector<glm::vec3>& positions,
                                                          const std::vector<std::uint32_t>& indices) {
    std::vector<glm::vec3> result;
    result.resize(positions.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::size_t idx0 = indices[i], idx1 = indices[i + 1], idx2 = indices[i + 2];
        glm::vec3 p0 = positions[idx0];
        glm::vec3 p1 = positions[idx1];
        glm::vec3 p2 = positions[idx2];

        glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));

        result[idx0] += normal;
        result[idx1] += normal;
        result[idx2] += normal;
    }

    for (auto& normal : result) {
        normal = glm::normalize(normal);
    }

    return result;
}

// TANGENT が無い glTF 用に、UV から接線を生成する（phase15 ③-4 / D-7）。
[[nodiscard]] std::vector<glm::vec4> generate_tangents(const std::vector<glm::vec3>& positions,
                                                       const std::vector<glm::vec3>& normals,
                                                       const std::vector<glm::vec2>& tex_coords,
                                                       const std::vector<std::uint32_t>& indices) {
    // 1. 各三角形ごとに接線と従法線を計算して蓄積
    std::vector<glm::vec3> tan_accum;
    std::vector<glm::vec3> bitan_accum;
    tan_accum.resize(positions.size());
    bitan_accum.resize(positions.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::size_t idx0 = indices[i], idx1 = indices[i + 1], idx2 = indices[i + 2];
        glm::vec3 p0 = positions[idx0], p1 = positions[idx1], p2 = positions[idx2];
        glm::vec2 uv0 = tex_coords[idx0], uv1 = tex_coords[idx1], uv2 = tex_coords[idx2];

        // 位置ベクトルの差分
        glm::vec3 edge1 = p1 - p0, edge2 = p2 - p0;

        // UV座標の差分
        glm::vec2 duv1 = uv1 - uv0, duv2 = uv2 - uv0;

        // UV 空間での三角形の面積の2倍（符号つき）
        float det = duv1.x * duv2.y - duv2.x * duv1.y;

        //   det は「UV 空間での三角形の面積の2倍」であって、縮退しているかの指標ではない。
        //   UV 面積は三角形数と UV の詰め方で決まる**スケール依存量**なので、
        //   絶対値で閾値を切ると「小さいだけの正常な面」を大量に捨てる。
        //
        //   ★ 閾値を 1e-12 へ下げるだけでは対症療法。閾値が絶対値である限り
        //     「正しい値」は存在せず、UV スケールの違うモデルで必ず再発する。
        //
        //   ★ 1/det を掛けないこと。後段で頂点ごとに正規化するので、必要なのは**向きだけ**。
        //     除算は (a) det→0 の特異点を作り、(b) UV 面積が小さい＝方向が最も当てにならない
        //     スリバー三角形を最大の重みで採用する（det=1.1e-5 なら重み 9 万倍）。
        //     符号だけ残せば両方が同時に消え、重みが辺の長さ（≒三角形の3D面積）に比例する
        glm::vec3 tan;
        glm::vec3 bitan;
        if (det == 0.0f) { continue; }          // 弾くのは真の縮退だけ
        float s = det < 0.0f ? -1.0f : 1.0f;
        tan   = s * ( duv2.y * edge1 - duv1.y * edge2);
        bitan = s * (-duv2.x * edge1 + duv1.x * edge2);
        //   ★ 符号は絶対に落とさないこと。det の符号は UV のミラーリングを表しており、
        //     捨てると左右対称モデルの片側だけ凹凸が反転する（tangent.w と同じ話）。
        //
        //   ★ なぜ除算が要らないのかを理解するのがこの修正の要点:
        //     「正規化が後段にあるから」。Lengyel 法の頑健版としてよく使われる形。

        // 頂点ごとに累積
        tan_accum[idx0] += tan;
        tan_accum[idx1] += tan;
        tan_accum[idx2] += tan;

        bitan_accum[idx0] += bitan;
        bitan_accum[idx1] += bitan;
        bitan_accum[idx2] += bitan;
    }

    // glTF規格に合わせた4次元接線ベクトルの作成 (X, Y, Z, W)
    // Wは従法線（Bitangent）の向き（反転フラグ: 1.0 または -1.0）
    std::vector<glm::vec4> result;
    result.resize(positions.size());

    // 2. グラム・シュミットの直交化とW成分の決定
    std::size_t fallback_count = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        glm::vec3 n = normals[i];
        glm::vec3 t = tan_accum[i];
        glm::vec3 b = bitan_accum[i];

        // 長さがゼロに近い場合の安全処理
        if (glm::length(t) < 1e-5) {
            fallback_count++;
            t = glm::abs(n[0]) < 0.9 ? glm::vec3{1.0, 0.0, 0.0} : glm::vec3{0.0, 1.0, 0.0};
        }

        // 法線に対して直交化: t = t - n * (n_dot_t)
        glm::vec3 t_ortho = t - n * glm::dot(n, t);

        // 規格化 (正規化)
        if (float norm = glm::length(t_ortho); norm > 1e-5) {
            t_ortho /= norm;
        } else {
            t_ortho = {1.0, 0.0, 0.0}; // フォールバック
        }

        // 向き（W成分）の計算。外積と元の従法線の方向を比較
        float w = glm::dot(glm::cross(n, t_ortho), b) >= 0.0 ? 1.0f : -1.0f;

        result[i] = {t_ortho, w};
    }

    if (fallback_count == 0) {
        spdlog::info("接線のフォールバックはありません。");
    } else {
        spdlog::debug("接線のフォールバック数: {}", fallback_count);
    }

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

// image ごとの「どちらの用途で要求されたか」（両方 true になり得る）
struct ImageUsage {
    bool srgb  = false;
    bool unorm = false;
};

//「image 添字 → 用途フォーマット」の表を作る。
[[nodiscard]] std::vector<ImageUsage> determine_image_formats(const tinygltf::Model& model) {

    std::vector<ImageUsage> result;
    std::size_t image_size = model.images.size();
    result.resize(image_size);
    for (auto& material : model.materials) {
        int idx0 = material.pbrMetallicRoughness.baseColorTexture.index;
        int idx1 = material.emissiveTexture.index;
        int idx2 = material.normalTexture.index;
        int idx3 = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        int idx4 = material.occlusionTexture.index;
        int source;
        if (idx0 != -1) {
            source = model.textures[idx0].source;
            if (source >= 0 && source < image_size) result[source].srgb = true;
        }
        if (idx1 != -1) {
            source = model.textures[idx1].source;
            if (source >= 0 && source < image_size) result[source].srgb = true;
        }
        if (idx2 != -1) {
            source = model.textures[idx2].source;
            if (source >= 0 && source < image_size) result[source].unorm = true;
        }
        if (idx3 != -1) {
            source = model.textures[idx3].source;
            if (source >= 0 && source < image_size) result[source].unorm = true;
        }
        if (idx4 != -1) {
            source = model.textures[idx4].source;
            if (source >= 0 && source < image_size) result[source].unorm = true;
        }
    }

    return result;
}

// 用途別に2本の対応表を返す（どちらも model.images.size() 個。
// 使われない用途の要素は無効ハンドルのまま＝ロードもしない）
struct ImageTables {
    std::vector<scene::TextureId> srgb;
    std::vector<scene::TextureId> unorm;
};

// model.images をすべて登録し、「glTF内の image 添字 → TextureId」の対応表を返す。
[[nodiscard]] ImageTables load_images(const tinygltf::Model& model,
                                      graphics::TextureRegistry& textures,
                                      const std::vector<ImageUsage>& usages) {

    ImageTables result;

    //   1. サイズの確保
    result.srgb.resize(model.images.size());
    result.unorm.resize(model.images.size());

    //   2. 各 image について to_rgba8 → TextureRegistry::load_from_pixels
    for (std::size_t i = 0; i < model.images.size(); i++) {
        if (!usages[i].srgb && !usages[i].unorm) continue; // 未参照 → 読まない

        auto& image = model.images[i];
        std::vector<unsigned char> pixels;
        if (!to_rgba8(image, pixels)) {
            spdlog::warn("load_images : rgba8への変換に失敗しました。");
            continue;
        }

        if (usages[i].srgb) {
            result.srgb[i]  = textures.load_from_pixels(pixels.data(),
                static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height),
                VK_FORMAT_R8G8B8A8_SRGB
            );
        }
        if (usages[i].unorm) {
            result.unorm[i] = textures.load_from_pixels(pixels.data(),
                static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height),
                VK_FORMAT_R8G8B8A8_UNORM
            );
        }
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
    //   1. texture_index < 0 は「このマテリアルはこのスロットのテクスチャを持たない」。
    //      ★ 異常ではないので警告は出さない。
    //        ここで市松模様の default_texture() を返すと、無地のモデル（Box.glb など）に
    //        模様が掛かってしまい、「テクスチャ無し」と「読み込み失敗」も区別できなくなる。
    //
    // ここで中立テクスチャを決めないこと。null ハンドルを返す。
    //   現状は5スロット共通で white_texture() を返しているが、白が中立なのは
    //   baseColor / metallicRoughness / occlusion / emissive（どれも乗算の恒等元）だけで、
    //   **normal スロットでは中立ではない**:
    //
    //       white = (255,255,255) → n_tex = (1,1,1)*2-1 = (1,1,1)
    //       N = normalize(TBN * (1,1,1)) = normalize(T + B + Ngeo)
    //         → ジオメトリ法線が接線方向へ **54.7°** 傾く
    //
    //   正しい中立値は flat_normal_texture() の (128,128,255) で、
    //   これなら n_tex ≈ (0.004, 0.004, 1.0) ＝傾き 0.32°（実質ゼロ）。
    //
    //   ★ MaterialRegistry::resolve_textures には既に用途ごとの正しい表があるのに、
    //     white_texture() が**有効なハンドル**なので contains() が true を返してしまい、
    //     flat_normal_texture() の枝が死にコードになっていた。
    //     フォールバックが2層にあり、手前の層が間違っていたという形。
    //
    //   ★ 中立値の決定は MaterialRegistry::resolve_textures に一本化する。
    //     そちらは glTF 以外の経路（main.cpp が手で組むマテリアル）も通るので、
    //     ローダー側に表を置くと「glTF 経由だけ正しい」という非対称が残る。
    //
    //   ★ scene::TextureId{} は index = kInvalidIndex (~0u) なので、
    //     TextureRegistry::contains() の第1条件 `id.index < slots_.size()` で
    //     確実に false になる。null 用の値を別に用意する必要は無い。
    //
    //   → return scene::TextureId{};
    if (texture_index < 0) {
        return scene::TextureId{};
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
                                                         const ImageTables& tables,
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
        // alphaMode で alpha_cutoff の意味を分ける（②-2 の約束: 0 なら MASK 無効）。
        //   "BLEND"  → alpha_cutoff = 0.0（transparent = true。下の LoadedMaterial で設定）
        //   "MASK"   → alpha_cutoff = material.alphaCutoff（既定 0.5）
        //   "OPAQUE" → alpha_cutoff = 0.0
        graphics::MaterialData material_data = {
            .base_color = base_color,
            .emissive =  emissive,
            .metallic = static_cast<float>(material.pbrMetallicRoughness.metallicFactor),
            .roughness = static_cast<float>(material.pbrMetallicRoughness.roughnessFactor),
            .alpha_cutoff = material.alphaMode == "MASK" ? static_cast<float>(material.alphaCutoff) : 0.0f,
        };

        spdlog::info("base_color: {}, {}, {}, {}", base_color.r, base_color.g, base_color.b, base_color.a);

        scene::MaterialTextures material_textures = {
            .albedo = resolve_texture(model, tables.srgb,
                material.pbrMetallicRoughness.baseColorTexture.index, textures),
            .normal = resolve_texture(model, tables.unorm,
                material.normalTexture.index, textures),
            .metallic_roughness = resolve_texture(model, tables.unorm,
                material.pbrMetallicRoughness.metallicRoughnessTexture.index, textures),
            .occlusion = resolve_texture(model, tables.unorm,
                material.occlusionTexture.index, textures),
            .emissive = resolve_texture(model, tables.srgb,
                material.emissiveTexture.index, textures)
        };

        // ★ glTF の metallicRoughness は G=roughness, B=metallic。occlusion は R チャンネル
        //   （ORM テクスチャなら metallicRoughness と同一画像を指すことが多い）。
        LoadedMaterial loaded_material = {
            .id = materials.add(material_data, material_textures),
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

    //   2. 属性を読む。
    //   ★ 3つの配列は**長さが揃っている前提**。揃っていなければ壊れた glTF
    //     warn + nullopt にする（短い方に合わせて読むと静かに壊れる）。
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> tex_coords;
    std::vector<glm::vec4> tangents;
    if (auto it = primitive.attributes.find("POSITION"); it == primitive.attributes.end()) {
        spdlog::warn("load_primitive : プリミティブにPOSITION属性がありません。");
        return std::nullopt;
    } else {
        positions = read_accessor<glm::vec3>(model, it->second);
    }

    //   インデックス
    std::vector<uint32_t> indices;
    if (primitive.indices < 0) {
        indices.resize(positions.size());
        std::iota(indices.begin(), indices.end(), 0u);
    } else {
        indices = read_indices(model, primitive.indices);
    }

    //   法線
    if (auto it = primitive.attributes.find("NORMAL"); it == primitive.attributes.end()) {
        normals = compute_flat_normals(positions, indices);
    } else {
        normals = read_accessor<glm::vec3>(model, it->second);
        if (positions.size() != normals.size()) {
            spdlog::warn("load_primitive : プリミティブのPOSITION属性とNORMAL属性の要素数が等しくありません。");
            return std::nullopt;
        }
    }

    //   テクスチャ座標
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

    //   TANGENT
    if (auto it = primitive.attributes.find("TANGENT"); it == primitive.attributes.end()) {
        // ★ 書式指定子 {} に対応する引数が無かったので頂点数を渡す形にした
        //   （引数無しの {} は fmt の実行時 format_error になりうる）。
        spdlog::info("load_primitive : プリミティブにTANGENT属性が無いので生成しました。頂点数 {}",
                     positions.size());
        tangents = generate_tangents(positions, normals, tex_coords, indices);
    } else {
        tangents = read_accessor<glm::vec4>(model, it->second);
        if (positions.size() != tangents.size()) {
            spdlog::warn("load_primitive : プリミティブのPOSISION属性とTANGENT属性の要素数が等しくありません");
            return std::nullopt;
        }
    }

    //   3. std::vector<Vertex> を組む（position / normal / tangent / uv を i 番目ずつ詰める）
    std::vector<graphics::Vertex> vertices;
    vertices.reserve(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        graphics::Vertex vertex = {
            .position = positions[i],
            .normal = normals[i],
            .tangent = tangents[i],
            .uv = tex_coords[i]
        };
        vertices.emplace_back(vertex);
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
    const std::vector<ImageUsage> image_usages = determine_image_formats(model);
    const ImageTables image_tables = load_images(model, textures, image_usages);
    const std::vector<LoadedMaterial> material_table =
        load_materials(model, image_tables, textures, materials);

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
    for (std::size_t i = 0; i < model.nodes.size(); i++) {
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
