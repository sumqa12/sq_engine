#pragma once

#include "sq/ecs/registry.hpp"

// InputManager / InputMap は参照を素通しするだけなので、ここでは前方宣言で足りる。
// これによりECSのヘッダがinputモジュール（ひいてはGLFW定数の世界）を引き込まずに済む。
namespace sq::input {
class InputManager;
class InputMap;
}  // namespace sq::input

namespace sq::ecs {

// 入力を必要とする更新処理の基底クラス（phase10プラン A-2）。
// SystemScheduler が保持する「同一の」InputManager / InputMap 参照を、すべてのControlSystemへ渡す。
// これにより各システムが個別にInputManagerを持つ必要がなくなり、入力状態のスナップショットが1つに揃う。
//
// System とは継承関係にしない（update のシグネチャが異なるため。
// SystemScheduler が2本のリストとして別々に保持する）。
class ControlSystem {
public:
    virtual ~ControlSystem() = default;

    ControlSystem(const ControlSystem&) = delete;
    ControlSystem& operator=(const ControlSystem&) = delete;

    virtual void update(const Registry& registry,
                        const input::InputManager& input,
                        const input::InputMap& map,
                        float dt) = 0;

protected:
    ControlSystem() = default;  // 抽象基底。派生クラスからのみ構築する
};

}  // namespace sq::ecs
