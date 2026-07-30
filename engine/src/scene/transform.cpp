#include "sq/scene/transform.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace sq::scene {

glm::mat4 Transform::model() const {
    // （phase12 手順1）
    glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
    glm::mat4 r = glm::mat4_cast(rotation);        // クォータニオン → 回転行列
    glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
    return t * r * s;
    // 掛ける順序に注意（列ベクトル規約なので、頂点には S → R → T の順に作用する）。
}

}  // namespace sq::scene
