#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_uv;

layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_proj; } camera;
// phase12 手順5: base_color を追加。★ frag と宣言を完全に一致させること
// （vert では base_color を使わないが、ブロック定義は揃える）。
// (phase13 ①-4): frag に合わせて `uint texture_index;` を末尾に追加する
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    uint texture_index;
} pc;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_uv;

void main() {
    gl_Position = camera.view_proj * pc.model * vec4(in_position, 1.0);
    frag_color = in_color;
    frag_uv = in_uv;
}
