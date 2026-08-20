#pragma once

#include <array>

#include <glm/glm.hpp>

namespace sq::scene {

// 軸に依存しない境界球（メッシュのローカル空間）。
// AABB より判定が単純（1回の内積と比較で済む）で、回転しても形が変わらないのが利点。
// その代わり実際の形状より大きめ（保守的）になるため、
// 「本当は画面外なのに描いてしまう」ことはあっても「見えるものを消す」ことはない。
struct BoundingSphere {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// view-projection 行列から抽出した視錐台の6平面。
// 平面は vec4(a, b, c, d) で ax + by + cz + d = 0 を表し、法線 (a, b, c) は**内側向き**に正規化する。
class Frustum {
public:
    // view_projection から6平面を抽出する（Gribb–Hartmann 法）。
    //
    // 原理: クリップ空間の座標 p_clip = M * p_world は -w <= x, y <= w かつ 0 <= z <= w を満たす。
    //   この不等式をワールド座標の線形式として書き直すと、そのまま平面の方程式になる。
    //
    // GLM は列優先（m[列][行]）なので、行ベクトルは row_i = (m[0][i], m[1][i], m[2][i], m[3][i])。
    //   left   = row3 + row0     （ x >= -w より）
    //   right  = row3 - row0     （ x <=  w より）
    //   bottom = row3 + row1
    //   top    = row3 - row1
    //   far    = row3 - row2     （ z <=  w より）
    //   near   = row2            ★ 深度 [0,1] の場合（ z >= 0 より）
    //
    // ★ 本プロジェクトは GLM_FORCE_DEPTH_ZERO_TO_ONE（深度 [0,1]）なので、near だけ教科書と違う。
    //   深度 [-1,1]（OpenGL 系）の教科書式 row3 + row2 を使うと near 平面がカメラの後ろに出て、
    //   「近くのものが消える／後ろのものまで描かれる」バグになる。
    //
    // 各平面は length(a, b, c) で割って正規化する。
    //   これをしないと dot + d が「符号付き距離」にならず、半径 radius との比較が成立しない。
    //   （内外の判定だけなら不要だが、球の判定には距離のスケールが要る）
    //
    // 注: Camera::view_projection() は proj[1][1] *= -1 でY反転している。
    //     これにより top/bottom の役割が入れ替わるが、6平面の集合としては同じなので判定に影響しない。
    [[nodiscard]] static Frustum from_view_projection(const glm::mat4& view_projection);

    // ワールド空間の球が視錐台と交差する（= 描画すべき）かを返す。
    //
    // 法線が内側向きなので、dot(plane.xyz, center) + plane.w は「その平面から内側へどれだけ離れているか」。
    // いずれか1つの平面について < -radius なら球は完全に外側なので false。
    // 6平面すべてを通過したら true（角の付近では厳密には外側でも true になり得るが、保守的なので問題ない）。
    [[nodiscard]] bool intersects(const glm::vec3& center, float radius) const;

private:
    // 並び順は from_view_projection の実装と合わせる（判定は全平面を回すので順序自体に意味は無い）。
    std::array<glm::vec4, 6> planes_{};
};

}  // namespace sq::scene
