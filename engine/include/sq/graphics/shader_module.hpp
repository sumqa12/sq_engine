#pragma once

#include <string>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// SPIR-V ファイルを読んで VkShaderModule を作る（phase16 ②-3）。
//
// GraphicsPipeline::load_shader_module（private static）を括り出したもの。
// ComputePipeline（②-3）からも使うため。
//
// ★ GraphicsPipeline 側の実装はここへ**移す**こと（コピーしない）。
//   GraphicsPipeline::load_shader_module は削除し、呼び出しをこの関数へ置き換える。
// ★ 呼び出し側が vkDestroyShaderModule すること（パイプライン作成後は不要になる）。
// ★ ファイルが開けない／サイズ 0 のときは throw すること。
[[nodiscard]] VkShaderModule load_shader_module(VkDevice device, const std::string& spv_path);

}  // namespace sq::graphics
