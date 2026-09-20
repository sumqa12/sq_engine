#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sq::scene {

// エンティティの**ローカル**変換を保持するコンポーネント。
// phase12 D-4: 合成済み mat4 を保持する形から TRS（平行移動・回転・スケール）を持つ形へ変更した。
// phase14 ④ / D-3: 行列キャッシュ（dirty フラグ）を入れ、親子階層に備えて「ローカル」であることを
//   名前で明示した（合成済みのワールド行列は scene::WorldTransform が持つ）。
//
// 回転はクォータニオンで持つ（オイラー角のジンバル問題を避ける）。
// Controller の yaw/pitch とは役割が別（あちらはカメラ操作の入力状態）。
//
// ★ struct から class へ変えた理由:
//   public メンバのままでは**書き換えを検知できない**（誰かが position を直接触っても
//   dirty を立てられず、キャッシュが古いまま使われる）。setter を唯一の入口にする。
//   代償として designated initializer（Transform{ .position = ... }）が使えなくなるので、
//   3引数のコンストラクタを用意してある。
class Transform {
public:
    Transform() = default;
    Transform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale);

    [[nodiscard]] const glm::vec3& position() const;
    [[nodiscard]] const glm::quat& rotation() const;
    [[nodiscard]] const glm::vec3& scale() const;

    // 書き換えると dirty_ が立ち、次の local() で再計算される。
    void set_position(const glm::vec3& v);
    void set_rotation(const glm::quat& q);
    void set_scale(const glm::vec3& v);

    // ローカル変換行列（T * R * S）。dirty のときだけ合成し直す。
    //
    // ★ const なのにキャッシュを書き換える（mutable）。
    //   Renderer::draw_frame が const Registry& を受けるため、const 経路から呼べないと
    //   再計算の機会が無くなる。「読み取りに見える操作でキャッシュだけ更新する」意図を
    //   型で示すのが mutable の役割（D-3）。
    [[nodiscard]] const glm::mat4& local() const;

    // 親のワールド行列が変わったときに、子側から明示的に汚す。
    void mark_dirty() const;
    [[nodiscard]] bool is_dirty() const;

private:
    glm::vec3 position_{0.0f};
    glm::quat rotation_{1.0f, 0.0f, 0.0f, 0.0f};  // 単位クォータニオン（w, x, y, z の順で初期化）
    glm::vec3 scale_{1.0f};

    mutable glm::mat4 cached_local_{1.0f};
    mutable bool dirty_ = true;   // 初回は必ず計算する
};

}  // namespace sq::scene
