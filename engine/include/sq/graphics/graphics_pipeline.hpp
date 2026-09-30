#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// パイプラインの可変設定（phase11 ①、phase16 ⓪-2 で拡張）。同じシェーダ・レイアウトから
// 用途ごとのパイプラインを作り分けるために使う。
//
// phase16 時点での使い分け:
//   不透明        : 既定のまま
//   半透明        : depth_write_enable = false, blend_enable = true   （back-to-front 描画前提）
//   シャドウ（①）: has_color_attachment = false, depth_bias_enable = true,
//                   cull_mode = VK_CULL_MODE_FRONT_BIT, frag_spv_path = ""
//   スカイボックス（②-9）:
//                   depth_write_enable = false, depth_compare_op = LESS_OR_EQUAL,
//                   has_vertex_input = false
//
// ★ 既定値は**すべて phase15 までの挙動**にすること（⓪-5 の完了条件）。
struct PipelineConfig {
    bool depth_write_enable = true;
    bool blend_enable = false;

    // -- ここから phase16 ⓪-2 で追加 --------------------------------

    // カラーアタッチメントの有無。false なら colorBlendState の attachmentCount = 0。
    //
    // ★ RenderPassConfig::has_color と**必ず揃える**こと。パイプラインは
    //   レンダーパスのサブパスと互換でなければならず、食い違うと
    //   vkCreateGraphicsPipelines がバリデーションエラーを出す（気付ける壊れ方）。
    bool has_color_attachment = true;

    // 深度バイアス（シャドウアクネ対策。①-8）。true なら
    //   rasterization_state_info.depthBiasEnable = VK_TRUE にし、
    //   VK_DYNAMIC_STATE_DEPTH_BIAS も動的ステートへ加える。
    // 値そのものは記録時に vkCmdSetDepthBias で渡す。
    //
    // ★ 動的にする理由: 適正値は解像度と正射影の範囲で変わる。パイプラインに焼くと
    //   1回試すたびにシェーダごと再ビルドすることになり、調整が現実的でなくなる。
    // ★ depthBiasEnable = VK_TRUE なのに vkCmdSetDepthBias を呼び忘れると、
    //   動的ステート未設定として未定義動作になる（バリデーションは指摘してくれる）。
    bool depth_bias_enable = false;

    // 面のカリング方向。
    //   本パス        : VK_CULL_MODE_BACK_BIT（既定。glTF は CCW が表）
    //   シャドウパス  : VK_CULL_MODE_FRONT_BIT にするとアクネが減る（①-8）。
    //                   ただし薄い板が影を落とさなくなる（peter-panning）
    //   スカイボックス: 立方体メッシュ方式なら FRONT。②-9 のフルスクリーン三角形方式なら NONE
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;

    // 深度比較演算。
    //   既定          : VK_COMPARE_OP_LESS（小さい深度 = 手前が勝つ）
    //   スカイボックス: VK_COMPARE_OP_LESS_OR_EQUAL
    //                   ★ 深度 1.0（最奥）で描くので、LESS のままだと
    //                     深度クリア値 1.0 と等しくなり**1ピクセルも残らない**
    VkCompareOp depth_compare_op = VK_COMPARE_OP_LESS;

    // 頂点入力の有無。false なら
    //   vertexBindingDescriptionCount = 0 / vertexAttributeDescriptionCount = 0 にする。
    // ②-9 のフルスクリーン三角形（gl_VertexIndex から頂点を組み立てる）で使う。
    //
    // ★ シャドウパスでは true のままでよい。頂点シェーダが position しか読まなくても、
    //   attribute を4本宣言しておくのは許される（逆に、宣言していない location を
    //   シェーダが読むのは不可）。stride を変えないので同じ頂点バッファを共有できる。
    bool has_vertex_input = true;
};

// シェーダーステージ、頂点入力レイアウト、ラスタライズ、ブレンディングなどを、
// 単一の不変の VkPipeline に組み込みます。Vulkan には、GL とは異なり、グローバルなレンダリング状態はありません。
class GraphicsPipeline {
public:
    // set_layouts: パイプラインレイアウトに組み込むディスクリプタセットレイアウト。
    //   index が set 番号に対応する（[0]=カメラUBO, [1]=マテリアル, [2]=ライティング環境）。
    //   phase12 手順3 で単体から複数へ、phase16 ⓪-4 で3本になった。
    //   呼び出し側が所有・破棄する（ここでは破棄しない）。
    //
    //   ★ phase16 D-3: **3本すべてのパイプラインで同じ配列を渡すこと**。
    //     set_layouts かプッシュ定数レンジが違うパイプライン同士は「レイアウト互換」で
    //     なくなり、パイプラインを切り替えた瞬間に**バインド済みのディスクリプタセットが
    //     黙って外れる**。シャドウパスが set=2 を使わなくても、レイアウトには含めておく。
    //
    // frag_spv_path: **空文字列ならフラグメントステージを作らない**（stageCount = 1）。
    //   深度専用のシャドウパスで使う（phase16 D-6）。
    //   ★ 既定引数にはしないこと。phase15 D-6 の VkFormat と同じ理由で、
    //     「書き忘れ」が黙って通ると、カラー出力の無いパイプラインが意図せず作られる。
    //
    // config: depth write / blend / カラーの有無 / depth bias / カリング / 深度比較 /
    //   頂点入力の有無（phase11 ①・phase16 ⓪-2。既定は不透明相当）。
    GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                      const std::string& vert_spv_path, const std::string& frag_spv_path,
                      const std::vector<VkDescriptorSetLayout>& set_layouts,
                      const PipelineConfig& config);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const;
    [[nodiscard]] VkPipelineLayout layout() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
