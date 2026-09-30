#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// イメージのレイアウト遷移（phase16 ②-1）。
//
// Texture::transition_image_layout（private static）を括り出したもの。
// キューブマップ（layerCount = 6）や、コンピュートでの書き込み（GENERAL）にも使うため、
// Texture の外から呼べる形にする。
//
// ★ Texture 側の実装はここへ**移す**こと（コピーしない）。Texture::transition_image_layout は
//   削除し、呼び出し箇所をこの関数へ置き換える。片方だけ直す事故の防止（phase15 の教訓）。
//
// 対象範囲: [base_mip_level, base_mip_level + level_count) × [base_array_layer, base_array_layer + layer_count)
//
// 対応する遷移パターン（old_layout と new_layout の組で分岐する）:
//   --- Texture（phase13 から） ---
//   UNDEFINED            -> TRANSFER_DST_OPTIMAL     src: 0              / TOP_OF_PIPE
//                                                    dst: TRANSFER_WRITE / TRANSFER
//   TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL     src: TRANSFER_WRITE / TRANSFER
//                                                    dst: TRANSFER_READ  / TRANSFER
//   TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL src: TRANSFER_READ  / TRANSFER
//                                                    dst: SHADER_READ    / FRAGMENT_SHADER | COMPUTE_SHADER
//   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL src: TRANSFER_WRITE / TRANSFER
//                                                    dst: SHADER_READ    / FRAGMENT_SHADER | COMPUTE_SHADER
//   --- phase16 ② で追加（コンピュートによる書き込み） ---
//   UNDEFINED            -> GENERAL                  src: 0              / TOP_OF_PIPE
//                                                    dst: SHADER_WRITE   / COMPUTE_SHADER
//   GENERAL              -> SHADER_READ_ONLY_OPTIMAL src: SHADER_WRITE   / COMPUTE_SHADER
//                                                    dst: SHADER_READ    / FRAGMENT_SHADER | COMPUTE_SHADER
//
// ★ SHADER_READ_ONLY への遷移の dst ステージに **COMPUTE_SHADER も含める**こと。
//   phase15 までは読み手がフラグメントシェーダだけだったが、② では
//     - HDR の 2D テクスチャを equirect_to_cube.comp が読む
//     - 環境キューブを irradiance.comp / prefilter.comp が読む（手順6）
//   ので、FRAGMENT だけだと「コンピュートが書き込み前のデータを読む」競合になる
//   （バリデーションの sync 検証を有効にしていないと気付けない）。
// ★ 未対応の組み合わせは throw すること（黙って通すとバリアが空になり、競合が見えなくなる）。
// ★ aspectMask は COLOR 固定でよい（深度イメージの遷移はレンダーパスが担う）。
void transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                             VkImageLayout old_layout, VkImageLayout new_layout,
                             std::uint32_t base_mip_level, std::uint32_t level_count,
                             std::uint32_t base_array_layer = 0, std::uint32_t layer_count = 1);

}  // namespace sq::graphics
