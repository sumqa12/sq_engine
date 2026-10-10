#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "sq/ecs/registry.hpp"
#include "sq/scene/shadow_constants.hpp"  // kCascadeCount（phase17 ②-4）

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
        float far_plane = 500.0f;

        // view-projection行列を返す。aspectはウィンドウの (幅 / 高さ)。
        // glm::perspectiveの深度範囲を[0,1]にするため、GLM_FORCE_DEPTH_ZERO_TO_ONE が必要（CMakeでプロジェクト全体に定義するのを推奨）。
        [[nodiscard]] glm::mat4 view_projection(float aspect) const;

        // view 行列だけ／projection 行列だけを返す（phase17 ②-1）。
        //   CSM は「カメラの視錐台を距離で輪切りにする」ために、view と「near/far を差し替えた projection」を
        //   別々に要る（②-3）。合成済みの view_projection だけでは輪切りが作れない。
        // ★ projection は Y 反転込み（view_projection と同じ。camera.cpp）。
        [[nodiscard]] glm::mat4 view() const;
        [[nodiscard]] glm::mat4 projection(float aspect) const;

        // アクティブカメラが1つも無いときに使うカメラ（phase17 ②-1）。
        // ★ ② の CSM は fov / near / far を個別に要るので、既定ビューも「行列」ではなく
        //   「Camera 構造体」で持つ（Renderer::resolve_camera の3段フォールバックの最後の段）。
        [[nodiscard]] static Camera fallback();
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
    //
    // phase16 ①-4 / D-3: light_view_projection を足して 96 → 160 バイト。
    // phase17 ②-4 / D-7: light_view_projection をカスケードの行列 kCascadeCount 本に置き換え、
    //   分割距離・テクセル幅・視線方向を足して 160 → 400 バイト。
    //   カスケードの行列は、シャドウパス（shadow.vert。プッシュ定数の番号で1本選ぶ）と
    //   本パス（triangle.frag。ビュー深度で段を選ぶ）の**両方**が読むので、プッシュ定数ではなくここに置く
    //   （phase17 D-3「同じ値を2経路で運ばない」）。
    // ★ シェーダ側の uniform CameraUBO ブロックは shaders/include/camera.glsl の1箇所。
    //   メンバの順・個数を変えたら必ず両方直すこと。kCascadeCount を変えたら camera.glsl の
    //   SQ_CASCADE_COUNT も変えること（sizeof は C++ 側でしか検査できない）。
    struct CameraUBO {
        glm::mat4  view_projection;                         // offset   0,  64
        glm::mat4  cascade_view_projection[kCascadeCount];  // offset  64, 256  各段の光源クリップ空間への変換（②-3）
                                                            //                  ★ std140 の mat4 配列は要素ごと 64 バイトで、C 配列と一致する
        glm::vec4  camera_position;                         // offset 320,  16  xyz=ワールド位置, w=未使用
        glm::vec4  camera_forward;                          // offset 336,  16  xyz=視線方向（正規化済み）, w=未使用
                                                            //                  frag のビュー深度 dot(p - cam, forward) に使う（②-2）
        glm::vec4  cascade_splits;                          // offset 352,  16  各段の**遠い側**のビュー深度（ワールド単位）
        glm::vec4  cascade_texel_sizes;                     // offset 368,  16  各段の1テクセルのワールド幅（法線オフセット。D-8）
        glm::uvec4 light_count;                             // offset 384,  16  x=有効ライト数
                                                            //                  y=デバッグフラグ（②-9 で bit0 = カスケード色分け）
                                                            //                  z, w=未使用
    };

    // CameraUBO::light_count.y のビット（デバッグフラグ。phase17 ②-9）。
    // ★ シェーダ側 SQ_DEBUG_CASCADE_COLORS（shaders/include/camera.glsl）と同じ値にすること。
    inline constexpr std::uint32_t kDebugFlagCascadeColors = 1u << 0;  // bit0: カスケードの段を色分けする

    // ★ cascade_splits / cascade_texel_sizes を vec4 にしているので、段数は 4 が上限。
    //   増やすなら float[] ではなく vec4 をもう1本足すこと（std140 では float 配列の要素が 16 バイトに膨らむ）。
    static_assert(kCascadeCount == 4, "cascade_splits / cascade_texel_sizes は vec4 1本で4段ぶん");
    static_assert(sizeof(CameraUBO) == 400, "std140 のレイアウトと一致させること");
    static_assert(offsetof(CameraUBO, camera_position) == 320, "シェーダ側のメンバ順と一致させること");
    static_assert(offsetof(CameraUBO, light_count) == 384, "シェーダ側のメンバ順と一致させること");
}  // namespace sq::scene
