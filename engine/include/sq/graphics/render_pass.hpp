#pragma once

#include <vulkan/vulkan.h>

namespace sq::graphics {

// レンダーパスの可変設定（phase16 ⓪-1）。PipelineConfig（graphics_pipeline.hpp）と同じ発想で、
// 「今の用途に決め打ち」だった部分だけを外から差し替えられるようにする。
//
// phase15 までの RenderPass は
//   「カラー1枚 + 深度1枚、カラーの finalLayout は PRESENT_SRC、深度は描いたら捨てる」
// で固定だった。シャドウパス（phase16 ①）はこれと3点違う:
//   1. カラーアタッチメントが無い（深度しか書かない。D-6）
//   2. 深度の finalLayout が DEPTH_STENCIL_READ_ONLY_OPTIMAL（この後 frag が読むため）
//   3. 深度の storeOp が STORE（中身が成果物なので捨ててはいけない）
//
// ★ 既定値は**すべて現在の挙動**にすること。既定構築した RenderPassConfig を渡した結果が
//   phase15 と1ビットでも違えば、それは ⓪ のリファクタが壊れている（⓪-5 の完了条件）。
struct RenderPassConfig {
    // false なら深度専用（subpass.colorAttachmentCount = 0）。
    // ★ GraphicsPipeline 側の PipelineConfig::has_color_attachment と**必ず揃える**こと。
    //   食い違うとパイプライン作成時にバリデーションエラーになる（これは気付ける壊れ方）。
    bool has_color = true;

    // 深度アタッチメントの最終レイアウト。
    //   本パス      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL（phase15 までと同じ）
    //   シャドウパス: VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    VkImageLayout depth_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // 深度アタッチメントの storeOp。
    //   本パス      : DONT_CARE（描き終わったら捨てる。phase15 までと同じ）
    //   シャドウパス: STORE
    // ★ ここを DONT_CARE のままシャドウパスを作ると、「影がノイズ・砂嵐になる」。
    //   バリデーションは何も言わない（仕様上は正しい指定なので）。一番忘れやすい1行。
    VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
};

// アタッチメントのセット（例：1つのカラーアタッチメント）および、
// グラフィックスパイプラインやフレームバッファで使用される読み込み／書き込みの挙動を定義します。
class RenderPass {
public:
    // config: phase16 ⓪-1 で追加。既定は「カラー+深度」（phase15 までの挙動）。
    // ★ has_color == false のとき color_format は使われないが、引数からは外さない
    //   （呼び出し側が VK_FORMAT_UNDEFINED を渡せばよい。オーバーロードを増やすより素直）。
    RenderPass(VkDevice device, VkFormat color_format, VkFormat depth_format,
               const RenderPassConfig& config = {});
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    [[nodiscard]] VkRenderPass handle() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
