#pragma once

#include "sq/ecs/entity_commands.hpp"
#include "sq/ecs/registry.hpp"
#include "sq/ecs/system.hpp"

namespace sq::scene {

// Lifetime の残り秒数を減らし、尽きたエンティティを子孫ごと消すシステム（phase17 ④-5）。
//
// EntityCommands（④-2）の最初の利用者。System の中から直接 registry.destroy を呼べない（registry が const）ので、
// 破棄は commands に積み、SystemScheduler::update の末尾でまとめて実行される（D-10）。
class LifetimeSystem final : public ecs::System {
public:
    LifetimeSystem() = default;

    // 1. view<Lifetime>().each で remaining -= dt
    //    ★ const Registry& から非 const の Lifetime& が取れるのは本エンジンの慣習（system.hpp の注記）。
    //      値の書き換えは構造変更ではないので、反復中でも安全。
    // 2. remaining <= 0 なら destroy_hierarchy(registry, commands, e) で破棄を積む
    //    ★ ここで registry.destroy を直接呼べないのが D-10 の効果（const なのでコンパイルエラーになる）。
    void update(const ecs::Registry& registry, ecs::EntityCommands& commands, float dt) override;
};

}  // namespace sq::scene
