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
//   現在の実装は file_size の例外を printf して**そのまま続行**し、サイズ 0 のモジュールを作ろうとする。
//   新しい .comp を足したのに CMake を再実行し忘れた、という場面で原因が見えなくなる。
[[nodiscard]] VkShaderModule load_shader_module(VkDevice device, const std::string& spv_path);

}  // namespace sq::graphics
