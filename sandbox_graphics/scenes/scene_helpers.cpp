#include "scene_helpers.hpp"

#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "sq/scene/camera.hpp"
#include "sq/scene/hierarchy.hpp"   // WorldTransform（phase14 ④）
#include "sq/scene/light.hpp"
#include "sq/scene/transform.hpp"

using namespace sq::scene;

namespace sandbox {

void update_oscillators(sq::ecs::Registry& registry, const float elapsed) {
    registry.view<Oscillator, Transform>().each([&](sq::ecs::Entity, Oscillator& o, Transform& t) {
        const float offset = o.amplitude * std::sin(o.speed * elapsed + o.phase);
        t.set_position(o.origin + glm::vec3(offset, 0.0f, 0.0f));
    });
    //   ★ 値の書き換えだけなので each の中で行ってよい（add / remove のような構造変更ではない）。
    //   ★ WorldTransform は描画の直前に TransformSystem::update が作り直すので、ここでは触らない。
}

void log_entity_and_asset_counts(sq::ecs::Registry& registry, sq::graphics::Renderer& renderer) {
    spdlog::info("entities={} meshes={} textures={} materials={}",
                 registry.entity_count(), renderer.meshes().alive_count(),
                 renderer.textures().alive_count(), renderer.materials().alive_count());
    //   ★ registry.entity_count() は非 const のメンバ関数（registry.hpp）。registry を非 const で受けているのはこのため。
}

sq::ecs::Entity create_camera(sq::ecs::Registry& registry,
                              ControlScheme scheme,
                              float x, float y, float z,
                              float tx, float ty, float tz) {
    const sq::ecs::Entity camera_entity = registry.create();
    auto position = glm::vec3(x, y, z);
    auto target = glm::vec3(tx, ty, tz);

    registry.add<Camera>(camera_entity,
        Camera {
            .position = position,
            .target = target,
        }
    );

    auto forward = glm::normalize(target - position);
    registry.add<Controller>(camera_entity,
        Controller {
            .scheme = scheme,
            .move_speed = 6.0f,
            .yaw = std::atan2(forward.z, forward.x),
            .pitch = std::atan2(forward.y, std::sqrt(forward.x * forward.x + forward.z * forward.z))
        }
    );

    return camera_entity;
}

sq::ecs::Entity create_directional_light(sq::ecs::Registry& registry,
                                         const glm::vec3& direction,
                                         const glm::vec3& color,
                                         float intensity,
                                         bool cast_shadows) {
    const sq::ecs::Entity entity = registry.create();

    // -Z が direction を向く回転を作る。
    //   mat3 は「列が x/y/z 軸」なので、+Z 軸には direction の逆向きを入れる。
    //   ★ glm::quatLookAt を使わないのは、それが gtx（実験的拡張）にあり
    //     GLM_ENABLE_EXPERIMENTAL の定義を要求するため。基底を手で組む方が素直で、
    //     「-Z 前方とは何か」も式の形で残る。
    const glm::vec3 axis_z = -glm::normalize(direction);

    // ★ 参照の上方向が z 軸とほぼ平行だと cross がゼロになり、回転が NaN になる。
    //   真上・真下からの光は「ほぼ平行」そのものなので、ここは必ず分岐が要る。
    const glm::vec3 reference_up = (std::abs(axis_z.y) > 0.99f)
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::vec3 axis_x = glm::normalize(glm::cross(reference_up, axis_z));
    const glm::vec3 axis_y = glm::cross(axis_z, axis_x);

    // 方向光は位置を使わないが、Transform は回転を運ぶ器として必要。
    registry.add<Transform>(entity,
        Transform(glm::vec3(0.0f), glm::quat(glm::mat3(axis_x, axis_y, axis_z)), glm::vec3(1.0f)));
    registry.add<WorldTransform>(entity, WorldTransform{});
    registry.add<Light>(entity,
        Light {
            .type = LightType::Directional,
            .color = color,
            .intensity = intensity,
            .cast_shadows = cast_shadows
            //   ★ 指示付き初期化子はメンバの宣言順に書くこと（range の後）。
        }
    );

    return entity;
}

sq::ecs::Entity create_point_light(sq::ecs::Registry& registry,
                                   const glm::vec3& position,
                                   const glm::vec3& color,
                                   float intensity,
                                   float range,
                                   bool cast_shadows) {
    const sq::ecs::Entity entity = registry.create();

    registry.add<Transform>(entity,
        Transform(position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)));
    registry.add<WorldTransform>(entity, WorldTransform{});
    registry.add<Light>(entity,
        Light {
            .type = LightType::Point,
            .color = color,
            .intensity = intensity,
            .range = range,
            .cast_shadows = cast_shadows
            //   ★ 指示付き初期化子は宣言順（range の後に cast_shadows）。
        }
    );

    return entity;
}

}  // namespace sandbox
