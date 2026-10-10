#version 450

// シャドウパス用の頂点シェーダ（phase16 ①-7）。
//
//  phase17 ①-2 で shadow.frag を持つようになった（アルファマスクの discard のため。uv と material_index を shadow.frag へ渡す）。
// ★ このファイルを足したら **CMake を再実行すること**（file(GLOB ...) は構成時にしか評価されない）。

layout(location = 0) in vec3 in_position;
// ★ normal / tangent は宣言しない（深度とαテストにしか使わない）。
//   パイプライン側の attribute_descriptions は 4 本のままでよい
//   （「宣言したが使わない location」は許される。逆は不可）。
//   stride（sizeof(Vertex)）を変えないこと。同じ頂点バッファを本パスと共有している。
// phase17 ①-2: αテスト用に uv を受け取る。★ location は triangle.vert と同じ 2。
layout(location = 2) in vec2 in_uv;

#include "camera.glsl"
#include "instance.glsl"
#include "shadow_push.glsl"   // phase17 ②-7: pc.indices.x = カスケード番号（D-3）

// phase17 ①-2: shadow.frag へ渡す（shadow.frag の in と location・型・flat を一致させること）。
layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_material_index;   // ★ flat 必須（整数は補間できない）

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    //   ★ 行列そのものは push しない（本パスの frag も同じ行列を読むので UBO に置いてある。D-3）。
    gl_Position = camera.cascade_view_proj[pc.indices.x] * inst.model * vec4(in_position, 1.0);
    //   ★ gl_InstanceIndex には firstInstance（= シャドウキャスタ区間の先頭）が既に足されている。
    frag_uv = in_uv;
    frag_material_index = inst.material_index;
}
