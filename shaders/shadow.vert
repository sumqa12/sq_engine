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

// ★ C++ 側 scene::CameraUBO と対（triangle.vert / triangle.frag と3箇所で揃える）。
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    mat4  light_view_proj;
    vec4  camera_position;
    uvec4 light_count;
} camera;

// ★ triangle.vert と同じ定義。C++ 側 InstanceData（80バイト）と対。
struct InstanceData {
    mat4 model;
    uint material_index;
};

layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

void main() {
    gl_Position = camera.light_view_proj * instances[gl_InstanceIndex].model * vec4(in_position, 1.0);
    //   ★ view_proj（カメラ）と取り違えると、シャドウマップにカメラから見た深度が書かれる。
    //   ★ gl_InstanceIndex には firstInstance（= シャドウキャスタ区間の先頭）が既に足されている。
}
