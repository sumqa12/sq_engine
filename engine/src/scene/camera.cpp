#include "sq/scene/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace sq::scene {

glm::mat4 Camera::view_projection(float aspect) const {
    glm::mat4 proj = glm::perspective(fov_y_radians, aspect, near_plane, far_plane);
    proj[1][1] *= -1;  // VulkanはY下向き。上下反転を補正
    glm::mat4 view = glm::lookAt(position, target, up);
    (void)aspect;
    return proj * view;
}

void set_active_camera(ecs::Registry& registry, ecs::Entity entity) {
    // TODO: entity を唯一のアクティブカメラにする（phase10プラン D-1）
    //  1. entity が Camera を持たない場合は spdlog::warn を出して何もせず return する
    //     （学習用サンドボックスで落としたくないため throw はしない）。
    //  2. 既存の ActiveCamera を集める:
    //       std::vector<ecs::Entity> previous;
    //       registry.view<ActiveCamera>().each([&](ecs::Entity e, ActiveCamera) { previous.push_back(e); });
    //     ★ ラムダ内で remove してはいけない。アーキタイプ間移動が起きて反復中のストレージが壊れる。
    //  3. ループを抜けてから外す:
    //       for (ecs::Entity e : previous) { registry.remove<ActiveCamera>(e); }
    //     （2で entity 自身も集まるので、ここで一度外れて 4 で付け直される。冪等になる）
    //  4. registry.add<ActiveCamera>(entity, {});
    (void)registry;
    (void)entity;
}

}  // namespace sq::scene
