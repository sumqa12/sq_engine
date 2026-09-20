#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "sq/graphics/buffer.hpp"

namespace sq::graphics {

    // 1頂点が持つデータ。位置（3D）・法線・テクスチャ座標を持つ。
    //
    // phase14 ③ / D-7: color を normal に置き換えた。
    //   color は phase12 で Material::base_color が入って以降**どこからも読まれていなかった**
    //   （vert が frag へ渡しているが frag が使っていない）。一方 glTF は頂点カラーを持たず
    //   NORMAL を持つことが多い。ライティング（phase15）の器を今のうちに用意しておく方が、
    //   後で頂点フォーマットを作り直さずに済む。
    //
    // ★ 移行時の罠: graphics_pipeline.cpp の attribute description は
    //   vec3 → vec3 で offset も同じなので、**直さなくてもコンパイルもバリデーションも通る**。
    //   気付かずに法線を色として使い続ける事故が起きやすい。
    //   シェーダ側の変数名（in_color → in_normal）も必ず直して、意味のずれを残さないこと。
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;  // テクスチャ座標（0..1）。graphics_pipelineのattribute location=2 と対応。
    };

    // 頂点データをGPUメモリに保持するRAIIラッパー。
    // バッファ生成・解放は基底クラスBufferが担い、本クラスは
    // 「構築時に頂点データを一度書き込む」「描画時にバインドする」責務のみ持つ。
    // （コピー禁止・handle()/size()は基底クラスから継承）
    class VertexBuffer : public Buffer {
    public:
        // phase11 ②③: DEVICE_LOCAL 化に伴い staging 転送用の queue_family/queue を、
        // サブアロケータ化に伴い GpuAllocator& を受ける。
        VertexBuffer(GpuAllocator& allocator, VkDevice device,
                     std::uint32_t queue_family, VkQueue queue,
                     const std::vector<Vertex>& vertices);
        // デストラクタは基底クラスに任せる（追加で解放するリソースは無い）

        // このバッファをコマンドバッファにバインドする（vkCmdBindVertexBuffers）。
        void bind(VkCommandBuffer command_buffer) const;

        [[nodiscard]] std::uint32_t vertex_count() const;

    private:
        std::uint32_t vertex_count_ = 0;
    };

    // インデックスデータをGPUメモリに保持するRAIIラッパー。Buffer基底の3例目。
    class IndexBuffer : public Buffer {
    public:
        // 組み込みジオメトリ用（uint16）。usage=INDEX_BUFFER_BIT|TRANSFER_DST, DEVICE_LOCAL
        IndexBuffer(GpuAllocator& allocator, VkDevice device,
                    std::uint32_t queue_family, VkQueue queue,
                    const std::vector<std::uint16_t>& indices);

        // phase14 ③ / D-6: glTF は頂点数が 65536 を超えると uint32 のインデックスを使う。
        // ★ 型が違うだけでバッファの作り方は同じ。違いは
        //   「バイト数が2倍になる」ことと「bind 時に渡す VkIndexType」だけ。
        //   オーバーロードにしてあるのは、呼び出し側が vector の型で自然に選べるようにするため。
        IndexBuffer(GpuAllocator& allocator, VkDevice device,
                    std::uint32_t queue_family, VkQueue queue,
                    const std::vector<std::uint32_t>& indices);

        // vkCmdBindIndexBuffer(..., index_type_) を呼ぶ。
        // ★ 固定で UINT16 を渡していた箇所を index_type_ に差し替えること。
        //   ここを直し忘れると、uint32 のメッシュが「インデックスが半分ずつずれて」
        //   ぐちゃぐちゃの三角形になる（クラッシュしないので気付きにくい）。
        void bind(VkCommandBuffer command_buffer) const;

        [[nodiscard]] std::uint32_t index_count() const;

    private:
        std::uint32_t index_count_ = 0;
        VkIndexType index_type_ = VK_INDEX_TYPE_UINT16;  // phase14 ③ で追加
    };

}  // namespace sq::graphics
