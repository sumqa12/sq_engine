#pragma once

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "sq/ecs/registry.hpp"

namespace sq::scene {

    // 透視投影カメラのECSコンポーネント。位置・注視点・画角などのパラメータを保持し、
    // view_projection()でview行列とprojection行列を合成して返す。
    // registry.view<Camera>()で取得し、最初に見つかったものをアクティブカメラとして使う想定。
    struct Camera {
        glm::vec3 position{0.0f, 0.0f, 3.0f};  // カメラの位置
        glm::vec3 target{0.0f, 0.0f, 0.0f};    // 注視点
        glm::vec3 up{0.0f, 1.0f, 0.0f};        // 上方向
        float fov_y_radians = glm::radians(45.0f);  // 垂直画角
        float near_plane = 0.1f;
        float far_plane = 50.0f;

        // view-projection行列を返す。aspectはウィンドウの (幅 / 高さ)。
        // glm::perspectiveの深度範囲を[0,1]にするため、GLM_FORCE_DEPTH_ZERO_TO_ONE が必要（CMakeでプロジェクト全体に定義するのを推奨）。
        [[nodiscard]] glm::mat4 view_projection(float aspect) const;

        static glm::mat4 default_view_projection(float aspect_ratio) {
            glm::mat4 view = glm::lookAt(
                glm::vec3(0.0f, 1.5f, 3.0f),  // カメラの位置
                glm::vec3(0.0f, 0.0f, 0.0f),  // 注視点
                glm::vec3(0.0f, 1.0f, 0.0f)   // 上方向
            );
            glm::mat4 projection = glm::perspective(glm::radians(45.0f), aspect_ratio, 0.1f, 10.0f);
            return projection * view;
        }
    };

    // 描画に使うカメラを示すタグ（データメンバなし。phase10プラン D-1）。
    // カメラエンティティが複数あっても、このタグが付いたものを Renderer が選ぶ。
    // タグ付きが複数ある場合はアーキタイプ走査順の最初の1つが選ばれる（決定的ではないため、
    // set_active_camera() を使って常に1つだけになるよう管理すること）。
    struct ActiveCamera {};

    // entity を唯一のアクティブカメラにする。他のエンティティからは ActiveCamera を外す。
    //
    // 注意（ECSの罠）: View::each() のラムダ内で add/remove するとアーキタイプ間移動が起きて
    // 反復中のストレージが壊れる。「先に対象Entityをvectorへ収集 → ループを抜けてから付け替え」
    // の2段構えで実装すること。
    void set_active_camera(ecs::Registry& registry, ecs::Entity entity);
    void set_controllable_camera(ecs::Registry& registry, ecs::Entity entity);
    void set_active_controllable_camera(ecs::Registry& registry, ecs::Entity entity);

    // 一致しない場合は空のEntityを返す (is_null()チェック必須)
    ecs::Entity get_active_camera(const ecs::Registry& registry);
    // 一致しない場合は空のEntityを返す (is_null()チェック必須)
    ecs::Entity get_controllable_camera(const ecs::Registry& registry);
    // 一致しない場合は空のEntityを返す (is_null()チェック必須)
    ecs::Entity get_active_controllable_camera(const ecs::Registry& registry);

    // Uniform Bufferへ転送するカメラデータのGPU側レイアウト。
    // シェーダーの layout(set=0, binding=0) uniform CameraUBO と一致させる。
    struct CameraUBO {
        glm::mat4 view_projection;
    };

}  // namespace sq::scene
