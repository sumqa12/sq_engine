#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in flat uint frag_texture_index;
layout(location = 3) in vec4 frag_base_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(textures[nonuniformEXT(frag_texture_index)], frag_uv) * frag_base_color;
}
