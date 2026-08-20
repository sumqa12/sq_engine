#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "sq/graphics/gpu_allocator.hpp"
#include "sq/graphics/texture.hpp"
#include "sq/scene/material.hpp"

namespace sq::graphics {

// TextureId → GPU実体（共有配列の添え字）のキャッシュ（phase12 D-3）。
// 同じ画像を何体のエンティティが参照してもロードは1回で済む（パスで重複を排除する）。
//
// テクスチャは起動時に確定し以後不変なので、ディスクリプタセット（set=1）は
// フレーム数とは無関係にテクスチャごと1個で足りる（phase12 D-5）。
// TextureId は entries_ の添字そのもの。
class TextureRegistry {
public:
    // descriptor_pool / material_set_layout / sampler は Renderer が所有し、ここでは破棄しない。
    // material_set_layout は「binding=0: COMBINED_IMAGE_SAMPLER, FRAGMENT」のレイアウト（set=1 用）。
    TextureRegistry(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                    std::uint32_t queue_family, VkQueue queue,
                    VkDescriptorPool descriptor_pool,
                    VkDescriptorSetLayout material_set_layout,
                    VkSampler sampler, std::uint32_t max_textures);
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // path の画像を読み込んで登録し TextureId を返す。
    // 同じ path が既に登録済みならロードせず既存のIDを返す（by_path_ で判定）。
    scene::TextureId load(const std::string& path);

    // 全テクスチャ共有の bindless ディスクリプタセット（set=1）。phase13 ①-3。
    // binding=0 が sampler2D の配列で、**TextureId がそのまま配列の添字**になる。
    // 内容は起動後に増えるだけ（既存要素は書き換わらない）なので、
    // フレーム先頭で1回バインドすれば以後ドロー中に触る必要がない。
    [[nodiscard]] VkDescriptorSet bindless_set() const;

    [[nodiscard]] bool contains(scene::TextureId id) const;

    // Material::albedo が無効／未登録のときに使う既定テクスチャ（最初に load したもの）。
    [[nodiscard]] scene::TextureId default_texture() const;

private:
    struct Entry {
        std::unique_ptr<Texture> texture;
    };

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;        // 所有しない（Renderer が所有）
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // 所有しない（Renderer が所有）
    VkSampler sampler_ = VK_NULL_HANDLE;                       // 所有しない（Renderer が所有）

    // 全テクスチャ共有の set=1（phase13 ①-3）。コンストラクタで1つだけ確保する。
    // プールから確保するので個別破棄は不要（プール破棄でまとめて解放される）。
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;

    std::vector<Entry> entries_;                                  // 添字が TextureId
    std::unordered_map<std::string, scene::TextureId> by_path_;   // 同一パスの再ロード防止
    scene::TextureId default_texture_ = scene::kInvalidTextureId;
    std::uint32_t max_textures_ = 0;  // 登録できるテクスチャ数の上限（プールの容量）
};

}  // namespace sq::graphics
