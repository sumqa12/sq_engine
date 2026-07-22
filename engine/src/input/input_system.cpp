#include "sq/input/input_system.hpp"

// キー/マウス定数のために GLFW を include してよい（InputManager 本体は GLFW 非依存のまま）。
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "sq/scene/camera.hpp"
#include "sq/scene/controller.hpp"

namespace sq::input {

void update_camera_control(const sq::ecs::Registry& registry, const InputManager& input, float dt) {
    // TODO: FreeFly カメラ操作（phase9プラン）
    //  registry.view<scene::ControlTarget, scene::Controller, scene::Camera>().each(
    //    [&](ecs::Entity, scene::Controller& ctrl, scene::Camera& cam) {
    //      // 1. マウス視線（右ボタン押下中のみ）
    //      if (input.is_mouse_down(GLFW_MOUSE_BUTTON_RIGHT)) {
    //          MouseDelta d = input.cursor_delta();
    //          ctrl.yaw   += static_cast<float>(d.dx) * ctrl.mouse_sensitivity;
    //          ctrl.pitch -= static_cast<float>(d.dy) * ctrl.mouse_sensitivity;
    //          // pitch を ±約89° にクランプ（真上/真下でのジンバルを避ける）
    //          const float limit = glm::half_pi<float>() - 0.01f;
    //          ctrl.pitch = glm::clamp(ctrl.pitch, -limit, limit);
    //      }
    //      // 2. yaw/pitch から forward・right を求める
    //      glm::vec3 forward{ cos(ctrl.pitch) * cos(ctrl.yaw), sin(ctrl.pitch), cos(ctrl.pitch) * sin(ctrl.yaw) };
    //      forward = glm::normalize(forward);
    //      const glm::vec3 world_up{ 0.0f, 1.0f, 0.0f };
    //      glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    //      // 3. WASD + Q/E で移動
    //      const float step = ctrl.move_speed * dt;
    //      if (input.is_down(GLFW_KEY_W)) cam.position += forward * step;
    //      if (input.is_down(GLFW_KEY_S)) cam.position -= forward * step;
    //      if (input.is_down(GLFW_KEY_D)) cam.position += right   * step;
    //      if (input.is_down(GLFW_KEY_A)) cam.position -= right   * step;
    //      if (input.is_down(GLFW_KEY_E)) cam.position += world_up * step;
    //      if (input.is_down(GLFW_KEY_Q)) cam.position -= world_up * step;
    //      // 4. 注視点を更新（向きを維持）
    //      cam.target = cam.position + forward;
    //      // 5.（任意）ホイールで fov_y_radians をズーム: cam.fov_y_radians を scroll_delta で増減しクランプ
    //    });
    (void)registry;
    (void)input;
    (void)dt;
}

}  // namespace sq::input
