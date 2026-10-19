#pragma once

#include <glm/glm.hpp>

#include "sq/ecs/entity.hpp"

// destroy_hierarchy の引数は参照を素通しするだけなので、前方宣言で足りる
// （hierarchy.hpp は Renderer など多くのファイルが include するので、重いヘッダを引き込まない）。
namespace sq::ecs {
class Registry;
class EntityCommands;
}  // namespace sq::ecs

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

// root とその全子孫の破棄を commands に積む（phase17 ④-4 / D-10）。
//
// ★ 読むだけなので registry は const。実際に消えるのは commands.flush のとき（SystemScheduler::update の末尾）。
// ★ ecs::EntityCommands に「再帰で消す」を持たせず scene 側に置くのは、Parent が scene の概念だから
//   （ecs は階層を知らない層のまま保つ。ecs → scene の依存を作らない）。
// ★ root が既に死んでいる場合は何もしない。
// ★ 子は Parent で親を指す片方向リンクしか持たない（phase14 ④）ので、子孫を知るには全 Parent を
//   1回走査するしかない。呼ばれたときだけ走るので、毎 tick のコストにはならない。
//   同じ tick に何十回も呼ぶなら、子の表を1回だけ作って使い回す形に変える（将来の最適化）。
void destroy_hierarchy(const ecs::Registry& registry, ecs::EntityCommands& commands, ecs::Entity root);
}  // namespace sq::scene
