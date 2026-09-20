#version 450
#extension GL_EXT_nonuniform_qualifier : require

const float kAmbient = 0.03;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    vec4  camera_position;
    uvec4 light_count;
} camera;


struct LightData {
    vec4 position_type;
    vec4 direction_range;
    vec4 color_intensity;
};
layout(std430, set = 0, binding = 2) readonly buffer LightBuffer {
    LightData lights[];
};

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
layout(location = 3) in vec3 frag_world_pos;

layout(location = 0) out vec4 out_color;

float getDistanceAttenuation(float distance, float lightRadius) {
    // 1. 逆二乗の基本
    float attenuation = 1.0 / (distance * distance + 0.001);

    // 2. 独自の計算で滑らかにゼロに落とす（smoothstepの代わり）
    float factor = distance / lightRadius;
    float factorSq = factor * factor;
    float window = clamp(1.0 - factorSq * factorSq, 0.0, 1.0);

    return attenuation * (window * window);
}

void main() {
    MaterialData m = materials[frag_material_index];
    vec3 N = normalize(frag_normal);
    vec3 V = normalize(camera.camera_position.xyz - frag_world_pos);
    vec3 L;
    float attenuation;
    float diffuse;
    float shininess = exp2(11.0 * (1.0 - m.roughness)) + 1.0;   // roughness 0 → 2049（鋭い）, 1 → 2（鈍い）

    // テクスチャの色を取得
    vec4 tex_color = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv);

    vec3 albedo = tex_color.rgb * m.base_color.rgb;
    vec3 result = kAmbient * albedo;

    for (uint i = 0; i < camera.light_count.x; ++i) {
        LightData light = lights[i];
        int type = int(light.position_type.w);
        vec3 pos = light.position_type.xyz;
        vec3 direction = light.direction_range.xyz;
        float range = light.direction_range.w;

        // 種別で L と減衰を分ける
        if (type == 0) {
            L = -direction;
            attenuation = 1.0;
        } else {
            L = normalize(pos - frag_world_pos);
            float distance = length(pos - frag_world_pos);
            attenuation = getDistanceAttenuation(distance, range);
        }
        diffuse  = max(dot(N, L), 0.0);
        float spec  = (diffuse > 0.0) ? pow(max(dot(N, normalize(L + V)), 0.0), shininess) : 0.0;

        vec3 diffuse_light = albedo * diffuse;
        vec3 specular_color = mix(vec3(0.04), albedo, m.metallic);

        float roughness_factor = 1.0 - m.roughness;
        vec3 specular_light = specular_color * spec * diffuse * roughness_factor;

        result += (diffuse_light + specular_light) * light.color_intensity.rgb * light.color_intensity.w * attenuation;
    }

    out_color = vec4(result, tex_color.a * m.base_color.a);
}
