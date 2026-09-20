#pragma once

#include <glm/glm.hpp>

#include "sq/ecs/entity.hpp"

namespace sq::scene {

// 親を指すコンポーネント。持たないエンティティはルート（phase14 ④ / D-4）。
//
// 「子のリスト」ではなく「親への1本のリンク」で持つ理由:
//   - ECS のストレージは固定サイズの要素が並ぶ配列なので、可変長の子リストを
//     コンポーネントに埋めると相性が悪い（ヒープ確保が体ごとに要る）
//   - 伝播は「子 → 親」方向にたどれれば足りる（後述の resolve_world が再帰で登る）
//
// ★ 循環（A→B→A）を作らないこと。作ると伝播が無限再帰になる。
//   TransformSystem 側で深さ上限による検出はするが、それは保険であって仕様ではない。
struct Parent {
    ecs::Entity entity;
};

// 合成済みのワールド変換行列（親の変換をすべて含んだもの）。
//
// なぜ Transform に持たせず別コンポーネントにするのか:
//   Transform は「自分のローカル変換」という入力、WorldTransform は「伝播の結果」という出力で、
//   寿命も更新主体も違う。分けておくと View<WorldTransform, MeshHandle> のように
//   「描画に必要なものだけ」を引けて、収集フェーズが軽くなる。
//
// ★ 描画対象には生成時に必ず付けること。伝播処理の中で後付けすると、
//   View::each() の反復中にアーキタイプ移動が起きてストレージが壊れる（D-4）。
//   （同じ注意書きが camera.hpp の set_active_camera にもある）
struct WorldTransform {
    glm::mat4 matrix{1.0f};
    bool resolved = false;   // このフレームで解決済みか（毎フレーム先頭でクリアする）
};

}  // namespace sq::scene
