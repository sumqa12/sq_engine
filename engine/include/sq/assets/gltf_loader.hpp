#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sq/graphics/material_registry.hpp"
#include "sq/graphics/mesh_registry.hpp"
#include "sq/graphics/texture_registry.hpp"
#include "sq/scene/asset_handle.hpp"

namespace sq::assets {

// glTF ファイルを読み込んだ結果（phase14 ③）。
//
// ★ ここに**エンティティは含まれない**。ローダーの責務は
//   「ファイル → 各レジストリへの登録 + ノード階層の記述」までで、
//   ECS への展開は呼び出し側が行う（③-5）。
//   こう分けておくと、同じモデルを何体も配置するときにロードが1回で済み、
//   「ロード」と「配置」を別のタイミングに置ける。
struct LoadedModel {
    // glTF のノード1つ分。
    struct Node {
        static constexpr std::size_t kNoParent = ~static_cast<std::size_t>(0);

        // 親ノードの**この配列内での添字**（ルートは kNoParent）。
        // ★ ecs::Entity ではない点に注意。エンティティはまだ存在しない。
        std::size_t parent = kNoParent;

        // ローカル変換（TRS）。glTF の matrix 形式は読み込み時に分解してここへ入れる。
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};

        // このノードが描画するメッシュとマテリアルの組。
        // ★ 1ノードが複数プリミティブを持つことがある（材質ごとに分かれている場合など）。
        //   本エンジンは「1エンティティ = 1メッシュ = 1マテリアル」を前提にしているので、
        //   展開側で子エンティティに分ける（③-5）。
        std::vector<std::pair<scene::MeshId, scene::MaterialId>> primitives;

        // primitives と同じ並びで、alphaMode == "BLEND" だったかを記録する。
        // ★ 透明かどうかは scene::Material（CPU側）のフラグなので、
        //   MaterialData には入らない。ここで運ばないと展開側が知る手段が無くなる。
        std::vector<bool> transparent;
    };

    std::vector<Node> nodes;
};

// path の .gltf / .glb を読み、メッシュ・テクスチャ・マテリアルを各レジストリへ登録して
// ノード階層を返す（phase14 ③）。
//
// 対応する: メッシュのプリミティブ（POSITION / NORMAL / TEXCOORD_0 / インデックス）、
//   baseColorTexture、baseColorFactor、metallicFactor / roughnessFactor、emissiveFactor、
//   ノード階層、TRS とマトリクス両方のノード変換、.gltf / .glb 両方。
//
// 対応しない（割り切り。D-6）: アニメーション、スキニング、モーフターゲット、カメラ、ライト、
//   KHR 拡張全般、スパースアクセサ、TEXCOORD_1 以降、頂点カラー。
[[nodiscard]] LoadedModel load_gltf(const std::string& path,
                                    graphics::MeshRegistry& meshes,
                                    graphics::TextureRegistry& textures,
                                    graphics::MaterialRegistry& materials);

}  // namespace sq::assets
