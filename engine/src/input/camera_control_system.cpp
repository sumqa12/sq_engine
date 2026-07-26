#include "sq/input/camera_control_system.hpp"

#include <glm/gtc/constants.hpp>

namespace sq::input {

namespace {
// ワールドの上方向。yaw/pitch から right を求めるときと、昇降移動に使う。
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
}  // namespace

void CameraControlSystem::update(const ecs::Registry& registry, const InputManager& input,
                                 const InputMap& map, float dt) {
    // ControlTarget + Controller + Camera を持つエンティティを走査し、
    // Controller::scheme で FreeFly / Orbit に振り分ける（phase10プラン A-4 / C）。
    registry.view<scene::ControlTarget, scene::Controller, scene::Camera>().each(
        [&](ecs::Entity, scene::ControlTarget, scene::Controller& controller, scene::Camera& camera) {
            switch (controller.scheme) {
                case scene::ControlScheme::FreeFly: update_free_fly(input, map, controller, camera, dt); break;
                case scene::ControlScheme::Orbit:   update_orbit(input, map, controller, camera, dt);   break;
            }
        });
}

glm::vec3 CameraControlSystem::forward_from(float yaw, float pitch) {
    // yaw/pitch を球面座標として前方ベクトルへ変換し normalize して返す。
    glm::vec3 forward{ cos(pitch) * cos(yaw), sin(pitch), cos(pitch) * sin(yaw) };
    return glm::normalize(forward);
}

void CameraControlSystem::apply_look(const InputManager& input, const InputMap& map,
                                     scene::Controller& controller) {
    // Action::CursorEnable がfalseのときだけ視線を回す（phase10プラン A-4）。
    if (map.is_down(input, Action::CursorEnable)) { return; }
    const auto [dx, dy] = input.cursor_delta();
    controller.yaw   += static_cast<float>(dx) * controller.mouse_sensitivity;
    controller.pitch -= static_cast<float>(dy) * controller.mouse_sensitivity;

    // pitch を ±約89° にクランプする（真上/真下でのジンバルロック回避。両スキーム共通）。
    constexpr float limit = glm::half_pi<float>() - 0.01f;
    controller.pitch = glm::clamp(controller.pitch, -limit, limit);
}

void CameraControlSystem::update_free_fly(const InputManager& input, const InputMap& map,
                                          scene::Controller& controller, scene::Camera& camera, float dt) {
    // FreeFly（phase9 の update_camera_control の本体をここへ移し、キー問い合わせを map 経由にする）
    apply_look(input, map, controller);

    glm::vec3 forward = forward_from(controller.yaw, controller.pitch);
    glm::vec3 right = normalize(cross(forward, kWorldUp));
    float step = controller.move_speed * dt;
    if (map.is_down(input, Action::MoveForward)) camera.position += forward * step;
    if (map.is_down(input, Action::MoveBackward)) camera.position -= forward * step;
    if (map.is_down(input, Action::MoveRight)) camera.position += right   * step;
    if (map.is_down(input, Action::MoveLeft)) camera.position -= right   * step;
    if (map.is_down(input, Action::MoveUp)) camera.position += kWorldUp * step;
    if (map.is_down(input, Action::MoveDown)) camera.position -= kWorldUp * step;

    camera.target = camera.position + forward;  // 向きを維持したまま注視点を追従させる

}

void CameraControlSystem::update_orbit(const InputManager& input, const InputMap& map,
                                       scene::Controller& controller, scene::Camera& camera, float dt) {
    // Orbit（phase10プラン C-2）
    apply_look(input, map, controller);

    // ホイールで距離ズーム:
    controller.orbit_distance -= static_cast<float>(input.scroll_delta()) * controller.zoom_speed;
    controller.orbit_distance = glm::clamp(controller.orbit_distance,
        controller.min_orbit_distance, controller.max_orbit_distance);

    glm::vec3 forward = forward_from(controller.yaw, controller.pitch);
    glm::vec3 right = normalize(cross(forward, kWorldUp));

    //     水平パン用に forward を水平面へ投影
    glm::vec3 flat = normalize(glm::vec3(forward.x, 0, forward.z));

    //  移動アクションで「注視点」camera.target をパンする（FreeFlyがpositionを動かすのと対照的）
    float step = controller.move_speed * dt;
    if (map.is_down(input, Action::MoveForward)) camera.position += flat * step;
    if (map.is_down(input, Action::MoveBackward)) camera.position -= flat * step;
    if (map.is_down(input, Action::MoveRight)) camera.position += right   * step;
    if (map.is_down(input, Action::MoveLeft)) camera.position -= right   * step;
    if (map.is_down(input, Action::MoveUp)) camera.position += kWorldUp * step;
    if (map.is_down(input, Action::MoveDown)) camera.position -= kWorldUp * step;

    //  5. カメラ位置を注視点から逆算する:
    camera.position = camera.target - forward * controller.orbit_distance;

    // 学習ポイント: FreeFly は position が主で target が従、Orbit は target が主で position が従。
    // 状態（yaw/pitch）は共通でも、どちらを起点に解くかが両スキームの本質的な違い。
}

}  // namespace sq::input
