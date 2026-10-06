#version 450

// シャドウパス用の頂点シェーダ（phase16 ①-7）。
//
// ★ 対になるフラグメントシェーダは**作らない**（D-6。深度しか書かない）。
//   shaders/ に shadow.frag を置かないこと。CMake が *.frag を GLOB しているので、
//   置けば勝手にコンパイルされて紛らわしくなる。
// ★ このファイルを足したら **CMake を再実行すること**（file(GLOB ...) は構成時にしか評価されない）。

layout(location = 0) in vec3 in_position;
// ★ normal / uv / tangent は宣言しない（深度しか要らない）。
//   パイプライン側の attribute_descriptions は 4 本のままでよい
//   （「宣言したが使わない location」は許される。逆は不可）。
//   stride（sizeof(Vertex)）を変えないこと。同じ頂点バッファを本パスと共有している。

#include "camera.glsl"
#include "instance.glsl"

void main() {
    gl_Position = camera.light_view_proj * instances[gl_InstanceIndex].model * vec4(in_position, 1.0);
    //   ★ view_proj（カメラ）と取り違えると、シャドウマップにカメラから見た深度が書かれる。
    //   ★ gl_InstanceIndex には firstInstance（= シャドウキャスタ区間の先頭）が既に足されている。
}
