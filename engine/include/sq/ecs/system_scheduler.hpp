#pragma once

#include <concepts>
#include <memory>
#include <utility>
#include <vector>

#include "sq/ecs/control_system.hpp"
#include "sq/ecs/system.hpp"

namespace sq::ecs {

// System / ControlSystem を所有し、登録順に update() を回す（phase10プラン A-3）。
//
// 実行順序は ControlSystem（入力反映）→ System（それ以外）に固定する。
// 入力で変化した状態を、同じ更新tick内でゲームロジック側が読めるようにするため。
class SystemScheduler {
public:
    SystemScheduler() = default;
    ~SystemScheduler() = default;

    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    // T を構築して所有権を受け取り、登録したシステムへの参照を返す
    // （呼び出し側でパラメータ調整や有効/無効の切替に使う）。
    // derived_from によるコンパイル時チェックで、誤った型の登録を弾く。
    template <typename T, typename... Args>
        requires std::derived_from<T, System>
    T& emplace_system(Args&&... args) {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *system;
        systems_.push_back(std::move(system));
        return ref;
    }

    template <typename T, typename... Args>
        requires std::derived_from<T, ControlSystem>
    T& emplace_control_system(Args&&... args) {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *system;
        control_systems_.push_back(std::move(system));
        return ref;
    }

    // 1更新tick分を実行する。input / map は全ControlSystemで共有される。
    void update(const Registry& registry,
                const input::InputManager& input,
                const input::InputMap& map,
                float dt);

private:
    std::vector<std::unique_ptr<ControlSystem>> control_systems_;
    std::vector<std::unique_ptr<System>> systems_;
};

}  // namespace sq::ecs
