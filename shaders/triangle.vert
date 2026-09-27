#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;   // xyz=接線, w=従法線の符号(±1)
//   ★ location は graphics_pipeline.cpp の attribute_descriptions[3] と必ず揃える。

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view_proj;
    vec4 camera_position;
    uvec4 light_count;
} camera;

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
// ワールド座標（点光の距離計算と視線ベクトルに要る）
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) out vec3 frag_tangent;
layout(location = 5) out vec3 frag_bitangent;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = camera.view_proj * world_pos;
    // ★ 法線はワールドへ「逆転置」で変換する。
    //   mat3(model) をそのまま掛けると、非等方スケール（親のスケール含む）が入ったとき
    //   法線が面に対して傾き、陰影が歪む。
    mat3 normal_matrix = transpose(inverse(mat3(inst.model)));
    frag_normal = normalize(normal_matrix * in_normal);
    // ★ inverse() は頂点ごとに走るので本来は重い。
    //   CPU 側で計算して InstanceData に積む案は phase16 の課題（後述）。
    frag_uv = in_uv;
    frag_material_index = inst.material_index;
    frag_world_pos = world_pos.xyz;

    // 接空間をワールドへ変換して frag へ渡す。
    vec3 T = normalize(mat3(inst.model) * in_tangent.xyz);
    //   ★ 接線は「面に沿った方向ベクトル」なので法線と変換の仕方が違う。
    //     ここで逆転置（normal_matrix）を使うと非等方スケール時に接空間が歪む。
    T = normalize(T - frag_normal * dot(frag_normal, T));  // 直交化（補間で崩れるぶんの予防）
    frag_tangent = T;
    frag_bitangent = cross(frag_normal, T) * in_tangent.w; // ★ w の符号を反映すること
}
