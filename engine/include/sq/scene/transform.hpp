#pragma once

#include <glm/glm.hpp>

namespace sq::scene {

// エンティティのワールド変換行列を保持するコンポーネント。
// 位置・回転・スケールは持たず、合成済みのモデル行列のみを保持する
// （変換ロジックは呼び出し側がglm::translate/rotate/scaleで組み立てる）。
struct Transform {
    glm::mat4 model{1.0f};
};

}  // namespace sq::scene
