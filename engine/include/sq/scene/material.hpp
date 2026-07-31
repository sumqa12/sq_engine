#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::scene {

// テクスチャの識別子。実体は graphics::TextureRegistry が所有する（phase12 D-1）。
// Texture はコピー禁止のRAII型なので、コンポーネントはIDのみを持つ（MeshId と同じ方針）。
using TextureId = std::uint32_t;
inline constexpr TextureId kInvalidTextureId = ~0u;

// 描画マテリアル（ECSコンポーネント。phase11 ① で新設、phase12 手順4 で拡張）。
// Material を持たないエンティティは既定マテリアル
// （既定テクスチャ・白・不透明）として扱う（後方互換）。
struct Material {
    // 貼るテクスチャ。kInvalidTextureId または未登録IDなら既定テクスチャが使われる。
    TextureId albedo = kInvalidTextureId;

    // テクスチャに乗算する色 tint。a < 1.0 で半透明の濃さを表現できる。
    // phase12 手順5 で push constant としてシェーダへ渡す（それまでは未使用）。
    glm::vec4 base_color{1.0f};

    // true なら半透明パスで back-to-front 描画する（phase11 ①）。
    bool transparent = false;

    // 将来: float alpha_cutoff / bool double_sided / vec3 emissive / metallic・roughness
};

}  // namespace sq::scene
