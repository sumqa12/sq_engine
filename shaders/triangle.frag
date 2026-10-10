#version 450
#extension GL_EXT_nonuniform_qualifier : require

#define PI 3.14159265359
#define EP 1e-5

#include "camera.glsl"
#include "light.glsl"
#include "material.glsl"

// set=2「ライティング環境」
//   ★ sampler2DShadow ではなく sampler2DArrayShadow。texture() の座標が vec3 → vec4（uv, 層番号, 比較する深度）になる。
//   ★ C++ 側のサンプラは compare_enable = true / CLAMP_TO_BORDER / 白ボーダー（⓪-3）。
//   ★ 宣言しただけでは「静的に使用」されないので、set=2 をバインドしていなくても害は無い。
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadow_cascades;

// 法線オフセットの大きさ（phase17 ②-8 / D-8）。**その段の1テクセルのワールド幅の何倍ずらすか**。
//   phase16 の kNormalBiasMax / Min は「NDC の z 空間の値」で、正射影の z 範囲が1つだから成立していた。
//   CSM は段ごとに z 範囲もテクセルの大きさも違うので、1つの定数では全段に合わない。
//   比較の**前に**ワールド座標を法線方向へずらし、ずらす量をテクセル幅（camera.cascade_texel_sizes[c]）に比例させる。
// ★ 出発点は 1.5（②-10 で調整）。近い段と遠い段で同時に縞が消えれば、比例が効いている。
const float kNormalOffsetScale = 1.5;

// IBL（phase16 ②-8）。C++ 側 write_environment_sets() が書く。
//   ★ 宣言するのは binding=1..3 に**キューブ / LUT の view を書いてから**にすること。
//     手順5までは binding=1 / 3 が白の 2D なので、samplerCube の binding=1 を読むと
//     「2D の view を samplerCube で読んだ」バリデーションエラーになる。
//     （宣言だけなら静的に使用されないので害は無い。読み始めた瞬間に問題になる）
layout(set = 2, binding = 1) uniform samplerCube irradiance_map;
layout(set = 2, binding = 2) uniform samplerCube prefiltered_map;
layout(set = 2, binding = 3) uniform sampler2D   brdf_lut;

// prefiltered の最大 mip（= EnvironmentMap::kPrefilteredMips - 1）。
// ★ C++ 側の mip 数と**必ず一致**させること。ずれると「roughness 1.0 だけ映り込みが鋭い」
//   （mip 数より小さい）か「途中から映り込みが変わらなくなる」（大きい）になる。
//   textureQueryLevels(prefiltered_map) - 1 で取れるが、定数の方が速い（②-8）。
const float kMaxReflectionLod = 4.0;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in flat uint frag_material_index;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) in vec3 frag_tangent;
layout(location = 5) in vec3 frag_bitangent;

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

// @brief IBL 用のフレネル（phase16 ②-8）
// @param NdotV     法線と視線の内積
// @param F0        垂直入射時の反射率
// @param roughness 粗さ
// @return 視角と粗さに応じた反射率
// @note  **F_Schlick とは別物**。粗い面ほど縁の反射を抑える。
//        F_Schlick を流用すると roughness 1.0 の面の輪郭が不自然に白く光る（縁取りされたように見える）。
//        直接照明は H（ハーフベクトル）を使うが、環境光には特定の H が無いので N·V で代用する。
vec3 F_SchlickRoughness(float NdotV, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}

// @brief world_pos のフラグメントが、方向光のカスケードシャドウの中にいるか（phase17 ②-8）
// @param world_pos  フラグメントのワールド座標
// @param geo_normal ジオメトリ法線（normalize(frag_normal)。法線マップ適用**前**）
// @param L          ライトへ向かうベクトル
// @return 1.0 = 照らされている、0.0 = 完全な影
//
// ★ geo_normal に法線マップ適用後の N を渡さないこと。法線オフセットが凹凸でぶれて、縞が消えにくくなる
//   （phase16 ①-7 の「アクネが消えないときはジオメトリ法線で試す」を、ここで最初から採用する）。
// 段の色分け表示の色（phase17 ②-9）。0 段目から 赤・緑・青・黄。
//   result（トーンマップ前の linear な色）に**掛ける**ので、1.0 を超えない淡い色にしておく
//   （純色 (1, 0, 0) を掛けると、緑や青の成分しか無い面が真っ黒になって形が読めなくなる）。
const vec3 kCascadeColors[SQ_CASCADE_COUNT] = vec3[](
    vec3(1.0, 0.4, 0.4),
    vec3(0.4, 1.0, 0.4),
    vec3(0.4, 0.4, 1.0),
    vec3(1.0, 1.0, 0.4)
);

// @brief world_pos がどのカスケード段に入るか（phase17 ②-9）
// @return 0..SQ_CASCADE_COUNT-1。影の最大距離（cascade_splits[最後]）より遠ければ -1
//
// ★ sample_cascade_shadow の手順1（段の選択）をここへ**移す**こと（コピーしない）。
//   色分け表示と影のサンプリングが別々に段を選ぶと、「色は緑なのに影は赤の段から引いている」という
//   デバッグ表示そのものが嘘をつく状態になる。両方がこの関数を呼ぶ形にする。
int select_cascade(vec3 world_pos) {
    //   1. ビュー深度で段を選ぶ
    float depth = dot(world_pos - camera.camera_position.xyz, camera.camera_forward.xyz);
    //        depth > cascade_splits[SQ_CASCADE_COUNT - 1] なら -1
    if (depth > camera.cascade_splits[SQ_CASCADE_COUNT - 1]) {
        return -1;
    }

    int cascade = SQ_CASCADE_COUNT - 1;
    for (int c = 0; c < SQ_CASCADE_COUNT; ++c) {
        if (depth < camera.cascade_splits[c]) {
            cascade = c;
            break;
        }
    }
    //   ★ 「カメラからの距離」ではなく「視線方向への射影」。距離で選ぶと段の境界が円弧になる。

    return cascade;
}

float sample_cascade_shadow(vec3 world_pos, vec3 geo_normal, vec3 L) {
    //   1. ビュー深度で段を選ぶ
    int cascade = select_cascade(world_pos);
    if (cascade == -1) {
        return 1.0;
    }

    //   2. 法線オフセット（D-8）
    vec3 offset_pos = world_pos + geo_normal * camera.cascade_texel_sizes[cascade] * kNormalOffsetScale
        * (1.0 - max(dot(geo_normal, L), 0.0));

    //   3. cascade_view_proj[c] で光源クリップ空間へ → uv / z（旧 sample_shadow と同じ。z は再マップしない）
    //   光源クリップ空間へ
    vec4 lp = camera.cascade_view_proj[cascade] * vec4(offset_pos, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    //      ★ 正射影なので w は常に 1。除算は phase17（透視）への備え。

    vec2 uv = ndc.xy * 0.5 + 0.5;
    float z = ndc.z;
    //      ★ z は再マップしない（GLM_FORCE_DEPTH_ZERO_TO_ONE で既に [0,1]）。
    //        OpenGL 由来の `* 0.5 + 0.5` を z にも書くと影が完全に消える。
    //      ★ uv.y も反転しない（シャドウパスと本パスで同じ向きのビューポート）。

    if (z > 1.0) {
        return 1.0; // ライトの far より遠い
    }
    //      uv の範囲外は CLAMP_TO_BORDER + 白ボーダーが 1.0 を返すので明示チェック不要。

    //   4. 3x3 PCF:
    //   ★ 段の境界で影の濃さが段差になるのは v1 の仕様（ブレンドは将来課題。②-8）。
    vec2 texel_uv = 1.0 / vec2(textureSize(shadow_cascades, 0).xy); // ★ 配列の textureSize は ivec3（z = 層数）
    float sum = 0.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        sum += texture(shadow_cascades, vec4(uv + vec2(x, y) * texel_uv, float(cascade), z));
    }

    return sum / 9.0;
}

void main() {
    MaterialData m = materials[frag_material_index];
    // マテリアルパラメータ取得
    // テクスチャがない場合はCBufferMaterialの定数値で代用
    vec4 albedo_sample = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv);

    float alpha = albedo_sample.a * m.base_color.a;
    if (is_masked_out(m, alpha)) { discard; }
    //   ★ ライティングの計算（下のテクスチャ読み・ライト 65 灯のループ）より**前**に置くこと。
    //     後ろに置いても結果は同じだが、捨てる画素に全部の計算をしてから捨てることになる。

    vec3 albedo = albedo_sample.rgb * m.base_color.rgb; // テクスチャ × 定数色
    // metallicRoughness / occlusion / emissive テクスチャを合成する。
    vec2 mr = texture(textures[nonuniformEXT(m.metallic_roughness_index)], frag_uv).gb;
    float metallic  = m.metallic  * mr.y;   // ★ B = metallic
    float roughness = clamp(m.roughness * mr.x, 0.03, 1.0);   // ★ G = roughness（逆に読むと粗い金属と滑らかな誘電体が入れ替わる）
    float ao = texture(textures[nonuniformEXT(m.occlusion_index)], frag_uv).r;
    vec3 emissive_tex = texture(textures[nonuniformEXT(m.emissive_index)], frag_uv).rgb;

    // 法線マップを適用する。ジオメトリ法線 frag_normal はTBN構築にのみ使う。
    vec3 n_tex = texture(textures[nonuniformEXT(m.normal_index)], frag_uv).xyz * 2.0 - 1.0;
    mat3 TBN = mat3(normalize(frag_tangent), normalize(frag_bitangent), normalize(frag_normal));
    vec3 N = normalize(TBN * n_tex);
    // ★ 凹凸が逆に見えるときに疑う順:
    //   1. in_tangent.w の符号（vert 側 ③-4）
    //   2. TBN の列の順序（T, B, N の順。転置して掛けていないか）
    //   3. 法線マップを sRGB で読んでいないか（D-6 / ③-2。UNORM で読むこと）
    //   4. glTF の UV 原点は左上。V の反転を二重にしていないか

    // 視線ベクトル（ピクセル → カメラ）
    vec3 V = normalize(camera.camera_position.xyz - frag_world_pos);

    // Cook-Torrance BRDF の各項を計算
    //   BRDF = (D * G * F) / (4 * NdotV * NdotL)
    float a = roughness * roughness; // α = roughness^2（GGXの慣習）
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0; // 直接照明用のk（IBLとは別式）

    // F0: 垂直入射時の反射率
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 result = vec3(0.0);

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

        // 影を落とすライト（int(light.shadow.x) >= 0 && light.position_type.w == 0）のときだけ係数を掛ける。
        float shadow = 1.0;
        if (int(light.shadow.x) >= 0 && type == 0) {
            shadow = sample_cascade_shadow(frag_world_pos, normalize(frag_normal), L);
        }
        //   ★ N はシェーディング法線（法線マップ適用後）でよいが、アクネが消えないときは
        //     ジオメトリ法線 normalize(frag_normal) で試すこと（法線マップの凹凸でオフセットがぶれる）。
        result += (diff + specular) * radiance * NdotL * shadow;
    }

    // IBL（環境光）を足す。
    float NdotV_ibl = max(dot(N, V), 0.0);
    vec3  F_ibl  = F_SchlickRoughness(NdotV_ibl, F0, roughness);
    vec3  kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);

    // 拡散
    vec3 irradiance  = texture(irradiance_map, N).rgb;
    vec3 diffuse_ibl = kD_ibl * albedo * irradiance;
    //     ★ / PI は書かない（irradiance.comp が PI を掛けて打ち消し済み）

    // 鏡面
    vec3 R = reflect(-V, N);
    //  ★ -V（カメラ → 表面）を N で反射する。V のまま渡すと映り込みが裏返る（②-10）
    vec3 prefiltered  = textureLod(prefiltered_map, R, roughness * kMaxReflectionLod).rgb;
    vec2 ab           = texture(brdf_lut, vec2(NdotV_ibl, roughness)).rg;
    vec3 specular_ibl = prefiltered * (F_ibl * ab.x + ab.y);

       result += (diffuse_ibl + specular_ibl) * ao;
    //   ★ ao は環境光にだけ掛ける（直接光のループには掛けない。phase15 の扱いのまま）。
    //   ★ シャドウ係数は IBL には掛けない（影の中でも空からの光は届く）。
    //     影の中が IBL で明るくなり、①で見た影が「薄く」なるのは**正しい変化**。

    // ★ トーンマップ（下の result / (result + 1)）は今までどおり**最後**に置く。
    result += m.emissive.rgb * emissive_tex;
    if ((camera.light_count.y & SQ_DEBUG_CASCADE_COLORS) != 0u) {
        int c = select_cascade(frag_world_pos);
        if (c >= 0) {
            result *= kCascadeColors[c];
        }
    }

    //   ★ トーンマップの**前**に掛けること（後に掛けると明るい所ほど色が飛んで見分けにくい）。
    //   ★ 影を落とすライトが無いフレームは cascade_splits が 0 なので塗られない
    //   ★ 影の最大距離より遠い画素（c == -1）は塗らない。ここで「影が出ない範囲」も見える。
    result = result / (result + 1.0);
    out_color = vec4(result, alpha);
}
