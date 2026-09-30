#version 450

// スカイボックス（phase16 ②-9）。
//
// ★ 環境キューブは set=2 binding=2 から読む。binding=2 は本来 prefiltered specular の枠だが、
//   手順5の間は**元の環境キューブ（512）**を入れておき、手順6で prefiltered に差し替える
//   （prefiltered の mip 0 = roughness 0 = 元の環境そのもの。②-9 の注記）。
//   ★ 手順6で差し替えると背景が 128×128 に粗くなる。気になったら set=2 に binding=4 として
//     元の環境キューブを足すこと（レイアウト・プール・write_environment_sets の3箇所）。

layout(set = 2, binding = 2) uniform samplerCube environment;

layout(location = 0) in vec3 in_direction;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 color = textureLod(environment, normalize(in_direction), 0.0).rgb;
    //     ★ normalize は frag 側で（補間でスケールが崩れるため）
    //     ★ textureLod で mip 0 を明示する（手順6で prefiltered に差し替えたとき、
    //       texture() だと画面の微分から mip が選ばれて背景がぼける）
    color = color / (color + vec3(1.0));   // ★ 本パスと同じトーンマップ（HDR をそのまま出すと白飛び）
    out_color = vec4(color, 1.0);
    //   ★ ガンマ補正は書かない。スワップチェーンが *_SRGB なので出力時にエンコードされる（phase15 ⓪）。
}
