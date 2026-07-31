#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
// phase12 手順3: テクスチャを set=1 へ分離する（set=0 はカメラUBO専用）。
// set=1 はマテリアル単位でバインドし直すため、binding は 0 から振り直す。
layout(set = 1, binding = 0) uniform sampler2D tex_sampler;

// phase12 手順5: マテリアルの色 tint。★ vert と宣言を完全に一致させること。
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    // テクスチャに base_color を乗算する。
    // base_color.a < 1.0 で半透明の濃さを表現できる（半透明パスのブレンドが効く）。
    out_color = texture(tex_sampler, frag_uv) * pc.base_color;
}
