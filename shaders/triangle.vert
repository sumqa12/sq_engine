#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;

layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_proj; } camera;
layout(push_constant) uniform PushConstants { mat4 model; } pc;

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = camera.view_proj * pc.model * vec4(in_position, 0.0, 1.0);
    frag_color = in_color;
}
