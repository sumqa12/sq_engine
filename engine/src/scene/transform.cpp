#include "sq/scene/transform.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace sq::scene {

Transform::Transform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
    : position_(position), rotation_(rotation), scale_(scale) {
    // dirty_ は既定で true。初回の local() で必ず合成される。
}

const glm::vec3& Transform::position() const { return position_; }
const glm::quat& Transform::rotation() const { return rotation_; }
const glm::vec3& Transform::scale() const { return scale_; }

void Transform::set_position(const glm::vec3& v) {
    // ★ 「値が変わっていなければ dirty_ を立てない」最適化も可能だが、
    //   vec3 の比較コスト vs T*R*S 合成コストの兼ね合いで決めること。
    position_ = v;
    dirty_ = true;
}

void Transform::set_rotation(const glm::quat& q) {
    rotation_ = q;
    dirty_ = true;
}

void Transform::set_scale(const glm::vec3& v) {
    scale_ = v;
    dirty_ = true;
}

const glm::mat4& Transform::local() const {
    //   dirty_ が false なら cached_local_ をそのまま返す（これがキャッシュの効果）。
    if (dirty_) {
        //   true なら phase12 手順1 と同じ合成を行ってから dirty_ = false にする
        glm::mat4 t = glm::translate(glm::mat4(1.0f), position_);
        glm::mat4 r = glm::mat4_cast(rotation_);        // クォータニオン → 回転行列
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale_);
        cached_local_ = t * r * s;
        //   掛ける順序に注意（列ベクトル規約なので、頂点には S → R → T の順に作用する）。

        dirty_ = false;
    }
    return cached_local_;
}

void Transform::mark_dirty() const {
    dirty_ = true;
}

bool Transform::is_dirty() const {
    return dirty_;
}

}  // namespace sq::scene
