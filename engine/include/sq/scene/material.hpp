#pragma once

#include <glm/glm.hpp>

namespace sq::scene {

// 描画マテリアル（ECSコンポーネント。phase11 ①）。今は半透明フラグのみを持つ。
// Material を持たないエンティティは「不透明」として扱う（後方互換）。
//
// 将来: ベースカラー tint・テクスチャID・両面描画フラグ・アルファカットオフ等の置き場にする。
struct Material {
    bool transparent = false;  // true なら半透明パスで back-to-front 描画する

    // glm::vec4 base_color{1.0f};  // 将来: 色 tint（push constant か UBO でシェーダへ渡す）
};

}  // namespace sq::scene
