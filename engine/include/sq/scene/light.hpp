#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace sq::scene {

// 光源の種別（phase15 ①）。
//
// ★ 値（0 / 1）は graphics::LightData::position_type.w にそのまま書き込まれ、
//   シェーダ側で int(w) として読まれる。**番号を入れ替えるとシェーダも同時に直すこと**。
//
// ★ スポットライトはこのフェーズでは入れない。円錐角と内外の減衰でパラメータが2つ増え、
//   「向きが正しいか」の確認がシャドウマップ（phase16）とセットの方がやりやすいため。
enum class LightType : std::uint32_t {
    Directional = 0,  // 平行光源（太陽）。位置を持たず、向きだけを使う
    Point       = 1,  // 点光源。位置と減衰半径を使う
};

// 光源のECSコンポーネント（phase15 ① / D-2）。
//
// ★ **位置も方向も持たない**。両方とも WorldTransform から取り出す:
//     位置 = glm::vec3(world.matrix[3])
//     向き = glm::normalize(-glm::vec3(world.matrix[2]))   // -Z 前方（glTF・OpenGL 系の慣習）
//
//   こう決めると phase14 ④ で作った親子階層がそのままライトに効く。
//   親エンティティに付ければライトが追従するので、「キャラクタが持つ松明」や
//   「車のヘッドライト」が追加のコード無しで書ける。
//   逆に Light 自身に座標を持たせると、Transform と二重管理になって必ずずれる。
//
// ★ ライトエンティティにも**生成時に Transform と WorldTransform を必ず付けること**
//   （phase14 D-4 の規約）。TransformSystem::update は <Transform, WorldTransform> を
//   回すので、付け忘れたライトは黙って原点の単位行列として扱われる（落ちないので気付きにくい）。
//
// ★ 親に非等方スケールが掛かっていると matrix[2] の長さが 1 でない。
//   向きを取り出すときは**必ず正規化する**こと。
struct Light {
    LightType type = LightType::Directional;

    // 光の色。★ linear 値で入れること（⓪ で決めた約束）。
    //   画像編集ソフトで見た sRGB の値をそのまま入れると、明るすぎる色になる。
    glm::vec3 color{1.0f};

    // 強度。color に乗算される。
    // ★ 0 以下のライトは収集フェーズで捨てる（GPU 側で無駄に回さないため）。
    float intensity = 1.0f;

    // Point のみ使う。この距離を超えたら寄与を 0 にする（減衰の打ち切り）。
    //
    // ★ 物理的には 1/d² の減衰は無限遠まで届くが、打ち切らないと
    //   全ライトが全ピクセルに効いてしまい、ライトを増やすほど際限なく重くなる。
    //   「どこまでを無視してよいか」を明示的に決めるための値。
    float range = 10.0f;
};

}  // namespace sq::scene
