#pragma once

#include "sq/ecs/entity.hpp"
#include "sq/ecs/registry.hpp"

namespace sq::scene {

// Transform（ローカル）と Parent から WorldTransform を求めるシステム（phase14 ④ / D-4）。
//
// 更新は「親 → 子」の順でなければならない（親のワールド行列が確定していないと
// 子を計算できない）。しかし ECS のエンティティはアーキタイプ順に並んでおり、
// 親子の順序とは無関係。毎フレーム トポロジカルソートするのは重い。
//
// → **メモ化付き再帰**にする。各エンティティについて「解決済みなら何もしない、
//   未解決なら親を先に解決してから自分を求める」を呼ぶだけで、順序を気にしなくてよくなる。
//   結果として各エンティティはフレームあたり1回しか合成されない。
class TransformSystem {
public:
    // 収集フェーズ（Renderer::draw_frame）より前に、毎フレーム1回実行する。
    //
    // 手順:
    //   1. registry.view<WorldTransform>().each(...) で resolved = false にする
    //   2. registry.view<Transform, WorldTransform>().each(...) で resolve_world(e, 0) を呼ぶ
    //
    // ★ ここで add/remove は絶対にしない（D-4 の罠。反復中のストレージが壊れる）。
    //   WorldTransform を持たないエンティティは、そもそもこの view に入らないので
    //   「後付けしよう」という発想自体を捨てること。
    static void update(ecs::Registry& registry);

private:
    // entity のワールド行列を求めて WorldTransform へ書き込み、resolved を立てる。
    //
    // 手順:
    //   1. 既に resolved なら何もしない（メモ化。ここが無いと深い階層で指数的に遅くなる）
    //   2. Parent を持たない → world = transform.local()
    //   3. Parent を持つ    → 親を先に resolve_world（再帰）してから
    //                          world = parent_world.matrix * transform.local()
    //   4. resolved = true
    //
    // ★ 循環参照の検出: depth が kMaxDepth を超えたら単位行列を入れて打ち切り、
    //   spdlog::error を出す。無限再帰でスタックを溢れさせない（D-4）。
    //   ★ 打ち切るときも resolved = true にすること。しないと同じフレーム内で
    //     何度も再帰し直して、エラーログが洪水になる。
    // ★ 親が死んでいる／Transform も WorldTransform も持たない場合は
    //   単位行列として扱う（壊れたハンドル対策）。
    static void resolve_world(ecs::Registry& registry, ecs::Entity entity, int depth);

    static constexpr int kMaxDepth = 64;
};

}  // namespace sq::scene
