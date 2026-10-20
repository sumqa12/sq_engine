#include "sq/scene/lifetime_system.hpp"

#include "sq/scene/hierarchy.hpp"   // destroy_hierarchy（phase17 ④-4）
#include "sq/scene/lifetime.hpp"

namespace sq::scene {

void LifetimeSystem::update(const ecs::Registry& registry, ecs::EntityCommands& commands, float dt) {
    registry.view<Lifetime>().each([&](const ecs::Entity entity, Lifetime& lifetime) {
        lifetime.remaining -= dt;
        if (lifetime.remaining <= 0) {
            destroy_hierarchy(registry, commands, entity);
        }
    });
    //   ★ each の中で destroy_hierarchy を呼んでよい（積むだけで、構造は変えない）。
    //     ただし destroy_hierarchy は呼ばれるたびに全 Parent を走査して子の表を作る（④-4）。
    //     同じ tick に大量に寿命が尽きるなら、尽きたものを vector に集めてから each の外で呼ぶ形でも同じ。
    //     今の検証シーン（数体）では気にしなくてよい。
}

}  // namespace sq::scene
