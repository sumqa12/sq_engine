#include "sq/scene/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "sq/scene/controller.hpp"

namespace sq::scene {

    glm::mat4 Camera::view_projection(float aspect) const {
        glm::mat4 proj = glm::perspective(fov_y_radians, aspect, near_plane, far_plane);
        proj[1][1] *= -1;  // VulkanはY下向き。上下反転を補正
        glm::mat4 view = glm::lookAt(position, target, up);
        return proj * view;
    }

    ecs::Entity get_active_camera(const ecs::Registry& registry) {
        return registry.view<Camera, ActiveCamera>().front();
    }

    ecs::Entity get_controllable_camera(const ecs::Registry& registry) {
        return registry.view<Camera, ControlTarget>().front();
    }

    ecs::Entity get_active_controllable_camera(const ecs::Registry& registry) {
        return registry.view<Camera, ActiveCamera, ControlTarget>().front();
    }

    void set_active_camera(ecs::Registry& registry, ecs::Entity entity) {
        // entity を唯一のアクティブカメラにする（phase10プラン D-1）
        // 1. entity が Camera を持たない場合は 何もしない
        if (!registry.has<Camera>(entity)) {
            spdlog::log(spdlog::level::warn, "set_active_camera : このエンティティは、カメラコンポーネントを持っていません。");
            return;
        }

        // 2. 既存の ActiveCamera を集める:
        std::vector<ecs::Entity> previous;
        registry.view<ActiveCamera>().each([&](ecs::Entity e, ActiveCamera) {
            previous.push_back(e);
        });

        // ★ ラムダ内で remove してはいけない。アーキタイプ間移動が起きて反復中のストレージが壊れる。
        // 3. ループを抜けてから外す:
        for (ecs::Entity e : previous) {
            registry.remove<ActiveCamera>(e);
        }

        //  4. ActiveCameraコンポーネントをつける
        registry.add<ActiveCamera>(entity, {});
    }

    void set_controllable_camera(ecs::Registry& registry, ecs::Entity entity) {
        // entatyを唯一の操作可能カメラにする
        if (!registry.has<Camera>(entity)) {
            spdlog::log(spdlog::level::warn, "set_controllable_camera : このエンティティは、カメラコンポーネントを持っていません。");
            return;
        }

        std::vector<ecs::Entity> previous;
        registry.view<ControlTarget>().each([&](ecs::Entity e, ControlTarget) {
            previous.push_back(e);
        });

        for (ecs::Entity e : previous) {
            registry.remove<ControlTarget>(e);
        }

        registry.add<ControlTarget>(entity, {});
    }

    void set_active_controllable_camera(ecs::Registry &registry, ecs::Entity entity) {
        set_active_camera(registry, entity);
        set_controllable_camera(registry, entity);
    }
}  // namespace sq::scene
