#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[];

// マテリアル本体（phase14 ①）。C++ 側 graphics::MaterialData（48バイト）と対になる。
// ★ メンバの順・型を1つでもずらすと、全マテリアルの見た目が崩れる。
struct MaterialData {
    vec4 base_color;
    vec4 emissive;
    float metallic;
    float roughness;
    float alpha_cutoff;
    uint albedo_index;
};

// set=1 = 「起動後は不変なアセット」（D-2）。テクスチャ配列と同居させる。
// set=0 と違ってフレーム複製が無いので、バッファは全体で1本。
layout(std430, set = 1, binding = 1) readonly buffer MaterialBuffer {
    MaterialData materials[];
};

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in flat uint frag_material_index;

layout(location = 0) out vec4 out_color;

void main() {
    MaterialData m = materials[frag_material_index];
    out_color = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv) * m.base_color;
    //   ★ nonuniformEXT は引き続き必要。1回の vkCmdDrawIndexed の中で
    //     インスタンスごとに albedo_index が変わる（= サブグループ内で非一様）ため。
    //     マテリアル経由になって添字の出所が変わっただけで、非一様であることは変わらない。
}
