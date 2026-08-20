#include "sq/scene/frustum.hpp"

#include <algorithm>

namespace sq::scene {

Frustum Frustum::from_view_projection(const glm::mat4& view_projection) {
    // (phase13 ⑤-1): 6平面を抽出する
    //  1. 行ベクトルを取り出す
    glm::vec4 row0 = {view_projection[0][0], view_projection[1][0], view_projection[2][0], view_projection[3][0]};
    glm::vec4 row1 = {view_projection[0][1], view_projection[1][1], view_projection[2][1], view_projection[3][1]};
    glm::vec4 row2 = {view_projection[0][2], view_projection[1][2], view_projection[2][2], view_projection[3][2]};
    glm::vec4 row3 = {view_projection[0][3], view_projection[1][3], view_projection[2][3], view_projection[3][3]};

    Frustum frustum{};
    //  2. planes_ を埋める
    frustum.planes_ = {
        row3 + row0, // left
        row3 - row0, // right
        row3 + row1, // bottom
        row3 - row1, // top
        row3 - row2, // far
        row2         // near
    };

    //  3. 各平面を正規化する
    for (auto& plane : frustum.planes_) {
        const float len = glm::length(glm::vec3(plane));
        plane /= len;
    }

    return frustum;
}

bool Frustum::intersects(const glm::vec3& center, float radius) const {
    return std::ranges::all_of(planes_, [&](const glm::vec4& p) {
        return glm::dot(glm::vec3(p), center) + p.w >= -radius;
    });
}

}  // namespace sq::scene
