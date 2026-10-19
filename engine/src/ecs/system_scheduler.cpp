#include "sq/ecs/system_scheduler.hpp"

// 参照を素通しするだけのヘッダ側とは違い、実体を扱うここでは定義が必要になる。
#include "sq/input/input_manager.hpp"
#include "sq/input/input_map.hpp"

namespace sq::ecs {

void SystemScheduler::update(Registry& registry,
                             const input::InputManager& input,
                             const input::InputMap& map,
                             float dt) {
    // ControlSystem を登録順に回す。
    for (const auto& system : control_systems_) {
        system->update(registry, commands_, input, map, dt);
    }

    // 続けて System を登録順に回す。
    for (const auto& system : systems_) {
        system->update(registry, commands_, dt);
    }
    // 順序を入れ替えないこと（入力→ゲームロジックの順で同一tick内に伝搬させる設計）。

    // 同期点(1箇所)
    commands_.flush(registry);
}

}  // namespace sq::ecs
