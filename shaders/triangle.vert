#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_proj; } camera;

// 型の定義。struct 自体はメモリ上の配置を持たないので layout 修飾子は付かない
// （付けると glslang が "useless application of layout qualifier" を出す）。
// 実際の配置は、これを使うブロック側の layout で決まる。
//
// phase14 ①: base_color / texture_index を廃止し material_index だけにした。
// C++ 側 InstanceData（80バイト）と対になる。
struct InstanceData {
   mat4 model;
   uint material_index;
};

// ★ std430 はここ（ブロック）に付ける。C++ 側 InstanceData の static_assert と対になる規則。
//   Vulkan GLSL では buffer ブロックの既定が std430 なので挙動は変わらないが、
//   何に合わせているかを明示しておく。
// ★ 末尾の可変長配列はブロックの最後のメンバにしか置けない。
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
   InstanceData instances[];
};

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
// phase14 ①: texture_index / base_color の受け渡しをやめ、マテリアル添字1本にした。
// frag 側は set=1 binding=1 の MaterialBuffer から直接 base_color を引く。
// ★ flat 必須（整数は補間できない）。
layout(location = 2) out flat uint frag_material_index;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    gl_Position = camera.view_proj * inst.model * vec4(in_position, 1.0);
    frag_normal = in_normal;
    frag_uv = in_uv;
    frag_material_index = inst.material_index;
}
