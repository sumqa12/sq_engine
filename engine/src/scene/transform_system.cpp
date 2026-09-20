#include "sq/scene/transform_system.hpp"

#include <spdlog/spdlog.h>

#include "sq/scene/hierarchy.hpp"
#include "sq/scene/transform.hpp"

namespace sq::scene {

void TransformSystem::update(ecs::Registry& registry) {
    //   1. 解決済みフラグを全クリアする
    registry.view<WorldTransform>().each(
        [](ecs::Entity, WorldTransform& wt) { wt.resolved = false; });

    //   2. 全エンティティを解決する
    registry.view<Transform, WorldTransform>().each(
        [&](ecs::Entity e, Transform&, WorldTransform&) { resolve_world(registry, e, 0); });
    //   ★ 2 のラムダの中で registry.add / remove を呼ばないこと（D-4）。
    //   ★ 1 と 2 で view の型が違う点に注意。1 は「WorldTransform を持つ全員」を
    //     クリアしたいので Transform を条件に入れない（Transform を持たない
    //     WorldTransform 持ちが居ても、古い resolved が残らないようにするため）。
}

void TransformSystem::resolve_world(ecs::Registry& registry, const ecs::Entity entity, const int depth) {
    //   0. registry.has<WorldTransform>(entity) でなければ return（親が非描画エンティティの場合）
    if (!registry.has<WorldTransform>(entity)) { return; }

    //   1. すでに解決済み
    auto& wt = registry.get<WorldTransform>(entity);
    if (wt.resolved) { return; }

    //   2. 深さチェック（循環参照の保険）
    if (depth > kMaxDepth) {
        spdlog::error("TransformSystem::resolve_world : 階層が深すぎます（循環参照の疑い）。");
        wt.matrix = glm::mat4(1.0f);
        wt.resolved = true;    // ★ 立てないと同じフレームで再突入しログが洪水になる
        return;
    }

    //   3. ローカル行列を得る（Transform が無ければ単位行列）
    const glm::mat4 local = registry.has<Transform>(entity)
        ? registry.get<Transform>(entity).local()
        : glm::mat4(1.0f);

    //   4. 親をたどる:
    if (registry.has<Parent>(entity)) {
        const ecs::Entity p = registry.get<Parent>(entity).entity;
        if (!p.is_null() && registry.is_alive(p)) {
            resolve_world(registry, p, depth + 1);          // ★ 先に親を解決する
            const glm::mat4 parent_world = registry.has<WorldTransform>(p)
                ? registry.get<WorldTransform>(p).matrix
                : glm::mat4(1.0f);
            wt.matrix = parent_world * local;               // ★ 順序: 親 * 子
        } else {
            wt.matrix = local;                              // 親が死んでいる → ルート扱い
        }
    } else {
        wt.matrix = local;                                  // ルート
    }

    wt.resolved = true;
    //   ★ 3 の wt は 4 の再帰でストレージが変わる可能性は無い（add/remove しないため）が、
    //     参照を長く持ち回るより、再帰の後に取り直す方が安全。
}

}  // namespace sq::scene
