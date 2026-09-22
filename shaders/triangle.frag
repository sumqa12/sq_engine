#version 450
#extension GL_EXT_nonuniform_qualifier : require

#define PI 3.14159265359
#define EP 1e-5

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

float saturate(float val) {
    return clamp(val, 0.0, 1.0);
}

float getDistanceAttenuation(float distance, float lightRadius) {
    // 1. 逆二乗の基本
    float attenuation = 1.0 / (distance * distance + EP);

    // 2. 独自の計算で滑らかにゼロに落とす（smoothstepの代わり）
    float factor = distance / lightRadius;
    float factorSq = factor * factor;
    float window = clamp(1.0 - factorSq * factorSq, 0.0, 1.0);

    return attenuation * (window * window);
}

// @brief 法線分布関数 (NDF): GGX / Trowbridge-Reitz
// @param NdotH 法線とハーフベクトルの内積
// @param a     粗さ（roughness）を2乗した値
// @return マイクロファセットの分布密度
// @note  粗さが高いほど分布が広がり、ぼやけた鏡面反射になる
float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * a2 - NdotH) * NdotH + 1.0;	// 2 mad
    return a2 / (PI * d * d);					// 4 mul, 1 rcp
}

// @brief 幾何減衰関数 (GSF): Smith / Schlick-GGX
// @param NdotV  法線と視線ベクトルの内積
// @param NdotL  法線とライト方向の内積
// @param k      roughness由来の係数 = (roughness+1)^2 / 8
// @return 自己遮蔽・自己シャドウの減衰係数 [0, 1]
// @note  視線側とライト側それぞれの遮蔽を掛け合わせる
float G_Smith(float NdotV, float NdotL, float k)
{
    // Schlick-GGX: G1(v) = NdotV / (NdotV * (1-k) + k)
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k); // 視線側
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k); // ライト側
    return ggx1 * ggx2;
}

// @brief フレネル項: Schlick近似
// @param HdotV  視線とハーフベクトルの内積
// @param F0        垂直入射時の反射率（誘電体=0.04、金属=アルベド色）
// @return 視角に応じた反射率
// @note  斜めから見るほど反射率が上がる（縁が光って見える現象）
vec3 F_Schlick(float HdotV, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
}

void main() {
    MaterialData m = materials[frag_material_index];
    // マテリアルパラメータ取得
    // テクスチャがない場合はCBufferMaterialの定数値で代用
    vec4 albedo_sample = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv);
    vec3 albedo = albedo_sample.rgb * m.base_color.rgb; // テクスチャ × 定数色
    float metallic = m.metallic;
    float roughness = clamp(m.roughness, 0.03, 1.0); // 0に近いと分母が発散するためクランプ

    // 法線
    vec3 N = normalize(frag_normal);

    // 視線ベクトル（ピクセル → カメラ）
    vec3 V = normalize(camera.camera_position.xyz - frag_world_pos);

    // Cook-Torrance BRDF の各項を計算
    //   BRDF = (D * G * F) / (4 * NdotV * NdotL)
    float a = roughness * roughness; // α = roughness^2（GGXの慣習）
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0; // 直接照明用のk（IBLとは別式）

    // F0: 垂直入射時の反射率
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 result = kAmbient * albedo;

    for (uint i = 0; i < camera.light_count.x; ++i) {
        LightData light = lights[i];
        int type = int(light.position_type.w);
        vec3 pos = light.position_type.xyz;
        vec3 direction = light.direction_range.xyz;
        float range = light.direction_range.w;

        vec3 L; // ライトベクトル
        vec3 H; // ハーフベクトル
        float attenuation;
        if (type == 0) {
            L = normalize(-direction);
            attenuation = 1.0;
        } else {
            L = normalize(pos - frag_world_pos);
            float distance = length(pos - frag_world_pos);
            attenuation = getDistanceAttenuation(distance, range);
        }
        H = normalize(L + V);

        float NdotL = saturate(dot(N, L)); // ランバート項（ライトの入射角）
        float NdotV = saturate(dot(N, V)) + EP; // ゼロ除算防止のためεを加算
        float NdotH = saturate(dot(N, H));
        float HdotV = saturate(dot(H, V)); // フレネル計算用

        float D = D_GGX(NdotH, a);
        float G = G_Smith(NdotV, NdotL, k);
        vec3 F = F_Schlick(HdotV, F0);

        // 鏡面反射項
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + EP);

        // 拡散反射項
        //   kD: エネルギー保存のため鏡面反射分(F)を引く
        //   金属は自由電子が光を吸収するため拡散反射なし (1-met)
        vec3 kD = (1.0 - F) * (1.0 - metallic);
        vec3 diff = kD * albedo / PI; // ランバート拡散

        // ライト放射輝度
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.w * attenuation;

        result += (diff + specular) * radiance * NdotL /* * shadow */;
    }

    result += m.emissive.rgb;
    result = result / (result + vec3(1.0));
    out_color = vec4(result, albedo_sample.a * m.base_color.a);
}
