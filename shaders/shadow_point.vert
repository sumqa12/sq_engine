#version 450

// 点光源のシャドウパス用の頂点シェーダ（phase17 ③-5 / D-9）。
//
// shadow.vert（CSM）との違いは行列の出どころだけ:
//   shadow.vert       : camera.cascade_view_proj[pc.indices.x]（本パスも同じ行列を読むので UBO。D-3）
//   shadow_point.vert : pc.view_proj（面の行列はシャドウパスしか読まないのでプッシュ定数。D-3）
// ★ このファイルを足したら **CMake を再実行すること**（file(GLOB ...) は構成時にしか評価されない）。

layout(location = 0) in vec3 in_position;
// ★ location は triangle.vert / shadow.vert と同じ 2。normal / tangent は宣言しない。
layout(location = 2) in vec2 in_uv;

#include "instance.glsl"
#include "shadow_push.glsl"   // pc.view_proj = この面の view-projection（③-3）

// ★ shadow_point.frag の in と location・型・flat を一致させること。
layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_material_index;   // ★ flat 必須（整数は補間できない）
layout(location = 2) out vec3 frag_world_pos;             // frag がライトからの距離を求めるのに要る（D-9）

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 world_pos = inst.model * vec4(in_position, 1.0);

    gl_Position = pc.view_proj * world_pos;
    frag_uv = in_uv;
    frag_material_index = inst.material_index;
    frag_world_pos = world_pos.xyz;
}
