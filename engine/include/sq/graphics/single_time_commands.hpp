#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// 1回きりの転送コマンド（レイアウト遷移・バッファ/イメージコピー等）を実行するRAIIヘルパ
// （phase10プラン E-1）。texture.cpp にべた書きされていた手順を切り出したもの。
//
// コンストラクタで TRANSIENT な一時コマンドプールと1本のプライマリコマンドバッファを作り、
// vkBeginCommandBuffer（ONE_TIME_SUBMIT）まで済ませて「記録可能」な状態にする。
// handle() に対して vkCmd* を記録し、submit_and_wait() で end → submit → vkQueueWaitIdle まで行う。
// デストラクタは submit_and_wait() 未呼び出しなら呼んでから、バッファとプールを破棄する。
//
// 注意: vkQueueWaitIdle でCPUをブロックする。起動時のアセット転送用であり、毎フレームの描画では使わない。
class SingleTimeCommands {
public:
    SingleTimeCommands(VkDevice device, std::uint32_t queue_family, VkQueue queue);
    ~SingleTimeCommands();

    SingleTimeCommands(const SingleTimeCommands&) = delete;
    SingleTimeCommands& operator=(const SingleTimeCommands&) = delete;

    // 記録先のコマンドバッファ。vkCmd* をこれに対して呼ぶ。
    [[nodiscard]] VkCommandBuffer handle() const;

    // 記録を終えて送信し、完了まで待つ。2回目以降の呼び出しは何もしない。
    void submit_and_wait();

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
    bool submitted_ = false;  // 二重submit防止
};

}  // namespace sq::graphics
