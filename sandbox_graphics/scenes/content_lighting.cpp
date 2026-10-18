#include "scene_content.hpp"

#include <random>

#include <spdlog/spdlog.h>

#include "scene_helpers.hpp"

namespace sandbox {

void add_sun(SceneBuildContext& ctx, const glm::vec3& direction, const float intensity) {
    // 太陽。方向光なので位置は使われず、向きだけが効く。loop() が Y 軸まわりに回す（phase16 ①-9）。
    ctx.state.sun = create_directional_light(ctx.registry,
        direction,                    // 光が進む向き。下向き成分が大きいほど真上からの光になる
        glm::vec3(1.0, 0.95, 0.9),    // ★ linear 値（phase15 ⓪ の約束。sRGB の値をそのまま入れない）
        intensity,
        true);                        // ★ 影を落とす（CSM）
}

void add_point_light_grid(SceneBuildContext& ctx) {
    // ---- 点光源（phase15 ① / phase17 ③）----
    //
    // ★ 色をランダムに振ってあるのは確認のため。ライトごとに違う色で照らされれば
    //   「位置が正しく渡っている」ことが一目で分かる。全部が同じ色に染まるなら、
    //   位置の取り出し（matrix[3]）か減衰の式を疑う。
    // ★ intensity が大きいのは IBL に埋もれないようにするため（phase17 ③-8 の注記）。
    //   1/d² で急速に落ちるうえ、屋外の空の IBL は拡散の環境光が 1 前後ある。
    // ★ 1列目の 5 個が影を落とす。kMaxPointShadows = 4 なので、5個目は警告が出て無視される（③-9）。
    std::uniform_real_distribution zero_one(0.0f, 1.0f);
    bool cast_shadows = true;
    for (int i = 0; i < 2; i++) {
        const auto f = static_cast<float>(i);
        for (int j = 0; j < 5; j++) {
            const auto k = static_cast<float>(j);
            const float r = zero_one(ctx.rng);
            const float g = zero_one(ctx.rng);
            const float b = zero_one(ctx.rng);
            spdlog::info("点光源色 r: {}, g: {}, b: {} ", r, g, b);
            create_point_light(ctx.registry, glm::vec3(f * 10 + 5, 4, k * 10 + 5), glm::vec3(r, g, b),
                               1200.0f, 20.0f, cast_shadows);
        }
        cast_shadows = false;
    }
}

}  // namespace sandbox
