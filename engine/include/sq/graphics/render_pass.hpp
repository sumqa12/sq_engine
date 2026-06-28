#pragma once

#include <vulkan/vulkan.h>

namespace sq::graphics {

// アタッチメントのセット（例：1つのカラーアタッチメント）および、
// グラフィックスパイプラインやフレームバッファで使用される読み込み／書き込みの挙動を定義します。
class RenderPass {
public:
    RenderPass(VkDevice device, VkFormat color_format);
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    [[nodiscard]] VkRenderPass handle() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
