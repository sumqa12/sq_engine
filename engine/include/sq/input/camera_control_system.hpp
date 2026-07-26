#pragma once

#include <glm/glm.hpp>

#include "sq/ecs/control_system.hpp"
#include "sq/input/input_manager.hpp"
#include "sq/input/input_map.hpp"
#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"

namespace sq::input {

// ControlTarget + Controller + Camera を持つエンティティに入力を適用するシステム（phase10プラン A-4）。
// phase9 の free関数 update_camera_control() を ecs::ControlSystem の派生クラスへ昇格させたもの。
// Controller::scheme に応じて FreeFly / Orbit を切り替える。
class CameraControlSystem : public ecs::ControlSystem {
public:
    CameraControlSystem() = default;

    void update(const ecs::Registry& registry, const InputManager& input,
                const InputMap& map, float dt) override;

private:
    // yaw/pitch から前方ベクトルを求める（両スキーム共通）。
    [[nodiscard]] static glm::vec3 forward_from(float yaw, float pitch);

    // 視線回転。Action::LookEnable 押下中のみ、カーソルデルタを yaw/pitch に反映し pitch をクランプする。
    static void apply_look(const InputManager& input, const InputMap& map, scene::Controller& controller);

    // カメラ位置そのものを動かし、注視点を追従させる（position が主）。
    static void update_free_fly(const InputManager& input, const InputMap& map,
                                scene::Controller& controller, scene::Camera& camera, float dt);

    // 注視点を動かし、カメラ位置を距離から逆算する（target が主）。ホイールで距離ズーム。
    static void update_orbit(const InputManager& input, const InputMap& map,
                             scene::Controller& controller, scene::Camera& camera, float dt);
};

}  // namespace sq::input
