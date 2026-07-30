#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sq::scene {

// エンティティのワールド変換を保持するコンポーネント。
// phase12 D-4: 合成済み mat4 を保持する形から TRS（平行移動・回転・スケール）を持つ形へ変更した。
// 従来の Position コンポーネントの役割は position が引き継ぐ（Renderer は Position を参照しない）。
//
// 回転はクォータニオンで持つ（オイラー角のジンバル問題を避ける）。
// Controller の yaw/pitch とは役割が別（あちらはカメラ操作の入力状態）。
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // 単位クォータニオン（w, x, y, z の順で初期化）
    glm::vec3 scale{1.0f};

    // T * R * S を合成したモデル行列を返す。
    // 行列キャッシュ + dirty フラグは将来課題（まずは毎回合成する）。
    [[nodiscard]] glm::mat4 model() const;
};

}  // namespace sq::scene
