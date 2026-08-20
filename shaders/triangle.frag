#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    uint texture_index;
} pc;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

void main() {
    // テクスチャに base_color を乗算する。
    // base_color.a < 1.0 で半透明の濃さを表現できる（半透明パスのブレンドが効く）。
    out_color = texture(textures[nonuniformEXT(pc.texture_index)], frag_uv) * pc.base_color;
}
