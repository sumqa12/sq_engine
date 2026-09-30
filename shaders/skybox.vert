#version 450

// スカイボックス（phase16 ②-9）。フルスクリーン三角形 + inverse(view_proj) でレイ方向を作る。
//
// ★ 頂点属性を持たない（PipelineConfig::has_vertex_input = false）。
//   vkCmdDraw(command_buffer, 3, 1, 0, 0) で gl_VertexIndex = 0, 1, 2 から頂点を作る。
// ★ 立方体メッシュ方式を採らない理由: 頂点／インデックスバッファ・メッシュ登録・
//   内側を向くカリング・near/far との干渉をすべて考えなくてよい。

// ★ C++ 側 scene::CameraUBO と対（triangle.vert / triangle.frag / shadow.vert と揃える。これで4箇所）。
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    mat4  light_view_proj;
    vec4  camera_position;
    uvec4 light_count;
} camera;

layout(location = 0) out vec3 out_direction;  // ワールド空間のレイ方向（未正規化）

void main() {
    vec2 uv  = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2)
    vec4 pos = vec4(uv * 2.0 - 1.0, 1.0, 1.0);    // ★ z = 1.0（最奥）。深度比較は LESS_OR_EQUAL
    gl_Position = pos;

    mat4 inv = inverse(camera.view_proj);  // ★ 頂点3つぶんしか走らないので許容
    vec4 w   = inv * pos;
    out_direction = w.xyz / w.w - camera.camera_position.xyz;
    //   ★ カメラ位置を引き忘れると、カメラを動かしたときに背景がついてくる（②-10）。
    //   ★ view_proj の Y 反転（camera.cpp）は inverse が打ち消すので、ここで補正は要らない。
}
