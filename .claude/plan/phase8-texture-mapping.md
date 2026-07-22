# C++ ゲームエンジン（学習目的）— Phase 8: テクスチャマッピング（stb_image + ステージングバッファ + combined image sampler）

## Context
Phase 7で深度バッファ + vec3頂点 + `IndexBuffer`（`Buffer`基底の3例目）+ 立方体のインデックス描画まで完了した（[phase7-depth-buffer-3d.md](phase7-depth-buffer-3d.md)参照）。現在は「頂点色（`in_color`）だけで着色した立方体を透視カメラ + 深度テストで描画する」状態。

Phase 8ではテクスチャマッピングを導入する。依存関係が一直線（**ステージングバッファ → `VkImage`テクスチャ → サンプラー → UV座標 → combined image sampler**）で、Phase 5のディスクリプタ（UBO）学習の自然な続きになる。

**方針（本セッションで確定した実装方式・王道ルート）**:
- 画像読み込み: **最初から stb_image**（vcpkgの`stb`ポート、PNG/JPG対応）
- GPU転送: **ステージングバッファ経由**（HOST_VISIBLEバッファ → `vkCmdCopyBufferToImage` → DEVICE_LOCAL image）。`Buffer`基底の転送用途（`TRANSFER_SRC`）の初例
- レイアウト遷移: **明示的パイプラインバリア**（UNDEFINED → TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL の2回遷移）
- 転送コマンド: **使い捨てコマンドバッファ（one-time submit）をグラフィクスキューへ**。専用トランスファーキューは将来課題
- ディスクリプタ: **combined image sampler**（`set=0, binding=1`、FRAGMENTステージ）。既存のカメラUBO（binding=0）にbindingを1つ追加する最小差分
- サンプラー: **1個の共有`VkSampler`を使い回す**（filter=LINEAR, addressMode=REPEAT）。
  **異方性フィルタリングは有効化する**（`device.cpp`で`samplerAnisotropy`機能を有効化・確定）
- テクスチャ画像: **プロジェクトルート `textures/` に配置**（確定。画像はユーザーが別途用意する）
- ミップマップ: **今回は作らない**（mipLevels=1）
- 深度バッファと同様、`Buffer::find_memory_type`が既に`public static`（Phase 7で昇格済み、`DepthImage`が利用中）なのでテクスチャ用`VkImage`のメモリ確保でも再利用する

例によってClaudeは宣言・骨格・TODOコメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 実装対象

### 1. 依存追加（stb_image）
- `vcpkg.json` の `dependencies` に `"stb"` を追加
- `engine/CMakeLists.txt`:
  - `find_package(Stb REQUIRED)` を追加し、`target_include_directories(sq_engine_lib PRIVATE ${Stb_INCLUDE_DIR})`
  - `STB_IMAGE_IMPLEMENTATION` の実体化は**1つの`.cpp`だけ**で行う。`texture.cpp` 冒頭で
    `#define STB_IMAGE_IMPLEMENTATION` してから `#include <stb_image.h>` する
  - **ついでに既存のバグ修正**: 現状 `add_library` のソース一覧に `src/graphics/buffer.cpp` が**2回**書かれている（12行目と19行目）。片方を削除する

### 2. 使い捨てコマンド（single-time commands）ヘルパ
テクスチャ転送用に、記録→送信→完了待ち→解放を1回で済ませる短命コマンドバッファを用意する。
**設計判断: `Texture`が内部で完結させ、既存の`CommandBuffers`（毎フレーム描画用プール）には依存させない**。
これにより`Texture`をコンストラクタ順序の早い段階（Device作成直後、ディスクリプタセット作成の前）で生成できる。

方式は2択。プランでは **(a) `Texture`内部に一時プール（transient pool）を持たせる** を採用:
- (a) `Texture`コンストラクタ内で `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` の一時`VkCommandPool`を作り、
  1本のコマンドバッファに「バリア→copy→バリア」を**まとめて記録**し、graphics queueへ1回submit → `vkQueueWaitIdle` → プール破棄。
  自己完結し、ディスクリプタ作成順序の制約を外せる（推奨）
- (b) `CommandBuffers`にプールのgetterを足して共有する案（`Texture`生成をコマンドバッファ作成後に移す必要あり）。今回は非採用

補足: テクスチャは起動時に一度読むだけなので、一時プールの生成/破棄コストは無視できる（学習優先）。

### 3. 新規 `Texture` クラス（`engine/include/sq/graphics/texture.hpp` / `src/graphics/texture.cpp`）
`VkImage` + `VkDeviceMemory` + `VkImageView` を所有するRAIIクラス。`DepthImage`と同系統（`VkImage`は`Buffer`基底とはAPIが別系統なので継承しない）。

```cpp
class Texture {
public:
    // path の画像を stb_image で読み込み、ステージングバッファ経由で
    // DEVICE_LOCAL な VkImage へ転送し、シェーダ読み取り可能なレイアウトにする。
    Texture(VkPhysicalDevice physical_device, VkDevice device,
            std::uint32_t graphics_queue_family, VkQueue graphics_queue,
            const std::string& path);
    ~Texture();  // view → image → memory の順で破棄

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] VkImageView view() const;

private:
    // oldLayout→newLayout のイメージバリアを記録する。
    // (UNDEFINED→TRANSFER_DST, TRANSFER_DST→SHADER_READ_ONLY の2ケースだけ扱う)
    static void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                        VkImageLayout old_layout, VkImageLayout new_layout);

    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};
```

コンストラクタの手順（TODO骨格に明記する内容）:
1. `stbi_load(path, &w, &h, &channels, STBI_rgb_alpha)`（強制RGBA=4ch）。失敗時は例外。`VkDeviceSize image_size = w * h * 4`
2. ステージング用に `StagingBuffer`（項目4）を生成し、ピクセルを書き込む。書き込み後 `stbi_image_free(pixels)`
3. `VkImageCreateInfo`（imageType=2D, extent={w,h,1}, mipLevels=1, arrayLayers=1,
   **format=`VK_FORMAT_R8G8B8A8_SRGB`**, tiling=OPTIMAL,
   usage=`VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`,
   samples=1, initialLayout=UNDEFINED, sharingMode=EXCLUSIVE）→ `vkCreateImage`
4. `vkGetImageMemoryRequirements` → `Buffer::find_memory_type(..., DEVICE_LOCAL)` → `vkAllocateMemory`
   （戻り値チェック）→ `vkBindImageMemory`
5. 一時コマンドバッファに以下をまとめて記録し、1回submit → wait（項目2の方式a）:
   - `transition_image_layout(UNDEFINED → TRANSFER_DST_OPTIMAL)`
   - `vkCmdCopyBufferToImage`（`VkBufferImageCopy`: imageSubresource aspect=COLOR, mip0, layer0, imageExtent={w,h,1}）
   - `transition_image_layout(TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL)`
6. `VkImageViewCreateInfo`（format=同上, aspectMask=`VK_IMAGE_ASPECT_COLOR_BIT`）→ `vkCreateImageView`
- `transition_image_layout`のstage/accessマスクの対応（バリアの肝・TODOに明記）:
  - UNDEFINED→TRANSFER_DST: srcAccess=0, dstAccess=`TRANSFER_WRITE`,
    srcStage=`TOP_OF_PIPE`, dstStage=`TRANSFER`
  - TRANSFER_DST→SHADER_READ_ONLY: srcAccess=`TRANSFER_WRITE`, dstAccess=`SHADER_READ`,
    srcStage=`TRANSFER`, dstStage=`FRAGMENT_SHADER`

### 4. `StagingBuffer` クラス（`Buffer`基底の4例目・転送元）
`mesh.hpp`/`mesh.cpp` に追加、または `buffer.hpp`/`buffer.cpp` の近くに置く。方針は`VertexBuffer`と同型:
- usage=`VK_BUFFER_USAGE_TRANSFER_SRC_BIT`、properties=`HOST_VISIBLE | HOST_COHERENT`
- `map()`→`std::memcpy`→`unmap()` で任意のバイト列を書き込む
- `Texture`がこれを使ってピクセルをGPUへ渡す

```cpp
// 転送元（TRANSFER_SRC）のHOST_VISIBLEバッファ。Buffer基底の4例目。
class StagingBuffer : public Buffer {
public:
    StagingBuffer(VkPhysicalDevice physical_device, VkDevice device,
                  const void* data, VkDeviceSize size);  // 構築時に data を書き込む
    // 追加リソースなし（破棄は基底に任せる）
};
```

補足: 頂点/インデックスバッファ自体のDEVICE_LOCAL化（現状は`VertexBuffer`/`IndexBuffer`がHOST_VISIBLE直書き）は**今回はやらない**。`StagingBuffer`はその土台にもなる（将来課題）。

### 5. 新規 `Sampler` クラス（`engine/include/sq/graphics/sampler.hpp` / `src/graphics/sampler.cpp`）
`VkSampler` のRAIIラッパー。全テクスチャで共有する1個を`Renderer`が所有する。

```cpp
class Sampler {
public:
    Sampler(VkPhysicalDevice physical_device, VkDevice device);
    ~Sampler();  // vkDestroySampler
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    [[nodiscard]] VkSampler handle() const;
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};
```

`VkSamplerCreateInfo`（TODO骨格）: magFilter/minFilter=LINEAR, addressModeU/V/W=REPEAT,
mipmapMode=LINEAR, mipLodBias=0, minLod=0, maxLod=0（ミップ無し）, borderColor=OPAQUE_BLACK,
unnormalizedCoordinates=FALSE。
- **異方性フィルタリング**: 学習ポイントとして有効化する。`anisotropyEnable=VK_TRUE`,
  `maxAnisotropy = VkPhysicalDeviceProperties::limits.maxSamplerAnisotropy`
  （`vkGetPhysicalDeviceProperties`で取得）。
  → **これには項目6のデバイス機能有効化が必須**。有効化しない場合は`anisotropyEnable=VK_FALSE`でも可

### 6. デバイス機能: `samplerAnisotropy` の有効化（`device.cpp`）
異方性を使うなら:
- `VkDeviceCreateInfo::pEnabledFeatures`（`VkPhysicalDeviceFeatures`）で `samplerAnisotropy = VK_TRUE`
- 厳密には`PhysicalDeviceSelector::is_suitable`で`vkGetPhysicalDeviceFeatures`の`samplerAnisotropy`を
  確認すべき（学習として一言コメント）。デスクトップGPUでは事実上常にサポートされる
- 異方性を使わない選択にする場合、この項目はスキップ可

### 7. `Renderer` のディスクリプタ拡張（combined image sampler 追加）
新規メンバ:
- `std::unique_ptr<Texture> texture_;`
- `std::unique_ptr<Sampler> sampler_;`

私有メソッド追加: `create_texture()`, `create_sampler()`

既存メソッドの変更:
- `create_descriptor_set_layout()`: bindingを**2つ**に
  - binding=0: `UNIFORM_BUFFER`, VERTEXステージ（既存のカメラUBO）
  - binding=1: `COMBINED_IMAGE_SAMPLER`, FRAGMENTステージ（新規）
  - `bindingCount=2`, `pBindings`=2要素配列
- `create_descriptor_pool()`: `VkDescriptorPoolSize`を**2種**に
  - `UNIFORM_BUFFER` × kFramesInFlight
  - `COMBINED_IMAGE_SAMPLER` × kFramesInFlight
  - `poolSizeCount=2`, `maxSets`は従来通りkFramesInFlight
- `create_descriptor_sets()`: 各フレームのセットに書き込みを**2つ**に
  - 既存: `VkDescriptorBufferInfo`（カメラUBO）→ binding=0
  - 追加: `VkDescriptorImageInfo{ .sampler=sampler_->handle(), .imageView=texture_->view(),
    .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }` → binding=1
  - `vkUpdateDescriptorSets` に2要素の`VkWriteDescriptorSet`配列を渡す
  - テクスチャ・サンプラーは共有なので、両フレームのセットが同じ`view`/`sampler`を指してよい

コンストラクタ順序（**`create_descriptor_sets()`の前**にtexture/samplerが必要**）:
- `...create_uniform_buffers()` → **`create_texture()` → `create_sampler()`** →
  `create_descriptor_pool()` → `create_descriptor_sets()` → `GraphicsPipeline`生成 → ...
- `create_texture()`は`physical_device_` / `device_->handle()` / `queue_family_indices_` /
  `device_->graphics_queue()` が揃っていれば呼べる（Device作成後ならいつでも可）

破棄順序（デストラクタ）: `vkDeviceWaitIdle`後、ディスクリプタプール破棄 → `texture_.reset()` /
`sampler_.reset()` → descriptor_set_layout破棄、の並び（RAIIなので`unique_ptr`メンバの逆順破棄に任せてもよいが、
プール破棄がセット参照を無効化する前後関係だけ意識する）。

**リサイズ非依存**: テクスチャ/サンプラー/ディスクリプタはスワップチェーンに依存しないため、
`recreate_swapchain()`での作り直しは不要（Phase 5のUBOと同じ理由）。

### 8. 頂点のUV追加（`mesh.hpp`）
```cpp
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 uv;   // 追加
};
```
- `graphics_pipeline.cpp` の頂点入力: attributeを**3つ**に
  - location=0: position, `VK_FORMAT_R32G32B32_SFLOAT`, offset=`offsetof(Vertex, position)`
  - location=1: color, `VK_FORMAT_R32G32B32_SFLOAT`, offset=`offsetof(Vertex, color)`
  - location=2: uv, `VK_FORMAT_R32G32_SFLOAT`, offset=`offsetof(Vertex, uv)`
  - `vertexAttributeDescriptionCount=3`、strideは`sizeof(Vertex)`のまま

### 9. 立方体メッシュのUV対応（`renderer.cpp` の `create_cube_mesh()`）
- **24頂点（面ごとに4頂点）構成**にする（8頂点共有だと面ごとのUVが破綻するため）。
  各面の4頂点に `uv = {0,0}/{1,0}/{1,1}/{0,1}` を割り当て、面ごとに0..1のテクスチャが貼られるようにする
- インデックスは面ごとに6個（2三角形）× 6面 = 36個
- Phase 7が既に24頂点構成ならUVを足すだけ。8頂点構成なら24頂点へ作り直す
- 巻き順（front face）とカリング設定（Phase 7でCULL_BACK有効化済みなら）に整合させる

### 10. シェーダー
`shaders/triangle.vert`:
```glsl
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_uv;          // 追加

layout(set = 0, binding = 0) uniform CameraUBO { mat4 view_proj; } camera;
layout(push_constant) uniform PushConstants { mat4 model; } pc;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_uv;       // 追加

void main() {
    gl_Position = camera.view_proj * pc.model * vec4(in_position, 1.0);
    frag_color = in_color;
    frag_uv = in_uv;
}
```
`shaders/triangle.frag`:
```glsl
layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;                       // 追加
layout(set = 0, binding = 1) uniform sampler2D tex_sampler; // 追加
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(tex_sampler, frag_uv);
    // 頂点色も混ぜたい場合: out_color = texture(tex_sampler, frag_uv) * vec4(frag_color, 1.0);
}
```

### 11. テクスチャ画像アセットの配置とコピー
- `textures/`（または`assets/`）ディレクトリを新設し、**ユーザーがPNG画像を1枚用意**する（例: `textures/statue.png`）
- 実行ファイル隣へコピーする必要がある。`sandbox_graphics/CMakeLists.txt` の**既存シェーダーコピーステップに倣って**、
  テクスチャを実行ファイル隣の`textures/`へコピーするカスタムコマンドを追加
- `Renderer`（またはmain）でロードパスを実行ファイル相対で指定（現状シェーダーと同じ流儀）

### 12. `sandbox_graphics/main.cpp`
- 描画ロジックは既存のまま（共有立方体 + Transform + カメラ旋回）でよい。テクスチャは`Renderer`内部で
  固定パスを読む前提なので、main側の変更は基本不要
- テクスチャの見え方を確認しやすいよう、立方体の回転・カメラ位置は現状維持

---

## 本セッションでの実施結果（骨格作成・完了）
CLAUDE.mdのルール（コード生成は宣言まで）に従い以下を作成。**骨格はすべてコンパイル可能な構成**を目指した
（新規クラスはTODOスタブ、既存ファイルの変更箇所はTODOコメント。テクスチャは未配線のため、
ユーザー実装完了まで見た目は従来の頂点色立方体のまま）。

- **新規**: [texture.hpp](../../engine/include/sq/graphics/texture.hpp) / [texture.cpp](../../engine/src/graphics/texture.cpp)（stb_image実体化・生成手順TODO）、
  [sampler.hpp](../../engine/include/sq/graphics/sampler.hpp) / [sampler.cpp](../../engine/src/graphics/sampler.cpp)（異方性込みのTODO）
- **`StagingBuffer`追加**: [buffer.hpp](../../engine/include/sq/graphics/buffer.hpp)（宣言）/ [buffer.cpp](../../engine/src/graphics/buffer.cpp)（基底初期化子リスト＋書き込みTODO）
- **宣言/構造変更**:
  - [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp): `Vertex`に`glm::vec2 uv`を追加
  - [renderer.hpp](../../engine/include/sq/graphics/renderer.hpp): `texture_`/`sampler_`メンバ、`create_texture()`/`create_sampler()`宣言、include追加
- **TODOコメント追加（既存実装は保持）**:
  - [renderer.cpp](../../engine/src/graphics/renderer.cpp): コンストラクタ（texture/sampler生成位置）、デストラクタ（reset）、
    `create_descriptor_set_layout`（binding=1）、`create_descriptor_pool`（2種目poolSize）、
    `create_descriptor_sets`（image write）、`create_cube_mesh`（24頂点UV化）、`create_texture`/`create_sampler`スタブ定義
  - [graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp): UV attribute（location=2）追加のTODO
  - [device.cpp](../../engine/src/graphics/device.cpp): `samplerAnisotropy`有効化のTODO
  - [triangle.vert](../../shaders/triangle.vert) / [triangle.frag](../../shaders/triangle.frag): UV/サンプラー追加のTODO（現行の動作コードは保持）
- **ビルド設定**:
  - [vcpkg.json](../../vcpkg.json): `stb`を追加
  - [engine/CMakeLists.txt](../../engine/CMakeLists.txt): `find_package(Stb)` + include追加、`texture.cpp`/`sampler.cpp`追加、
    **重複していた`buffer.cpp`の一方を削除（バグ修正）**
  - [sandbox_graphics/CMakeLists.txt](../../sandbox_graphics/CMakeLists.txt): `textures/`を実行ファイル隣へコピーするステップ追加
  - `textures/`ディレクトリを新設（[README.md](../../textures/README.md)。画像はユーザーが配置）

## ユーザーが実装する本体（TODO・推奨順）
1. `StagingBuffer`コンストラクタ（`map`→`memcpy`→`unmap`、`<cstring>`）
2. `Sampler`（`vkCreateSampler`。異方性のため先に`device.cpp`の`samplerAnisotropy`有効化）
3. `Texture`（stb_image読込→ステージング→バリア→copy→バリア→view、一時コマンドプール）
4. `renderer.cpp`のディスクリプタ拡張（layout/pool/sets binding=1）＋コンストラクタ/デストラクタでのtexture/sampler生成・破棄
5. `Vertex`のUV・`graphics_pipeline.cpp`のattribute・`create_cube_mesh`の24頂点UV化
6. シェーダー（`triangle.vert`/`triangle.frag`）のUV・サンプラー配線
7. `textures/`にPNG配置 → `create_texture()`のパス指定

## 実装順序（推奨）
1. **依存とビルドを先に通す**（項目1）: `vcpkg.json`に`stb`追加 → `find_package(Stb)` → `buffer.cpp`重複除去。
   空の`texture.cpp`/`sampler.cpp`を足してビルドが通る状態にする
2. **`StagingBuffer`**（項目4）: `VertexBuffer`と同型。単体では見た目に影響しない
3. **`Sampler`**（項目5）: `VkSampler`生成。異方性を使うなら先に`device.cpp`（項目6）
4. **`Texture`**（項目3・2）: stb_image読み込み → ステージング → バリア → copy → バリア → view。
   ここが本フェーズの核。まだシェーダに繋がないのでこの時点では見た目不変
5. **ディスクリプタ拡張**（項目7）: layout/pool/setsにbinding=1を追加。texture/samplerをRendererが所有
6. **UV + シェーダー**（項目8・9・10）: `Vertex`にuv追加、頂点入力3属性、立方体24頂点にUV、シェーダで`texture()`。
   立方体にテクスチャが貼られる
7. **アセット配置**（項目11）: PNG用意 + CMakeコピーステップ

## 検証方法
- ビルドはCLion同梱cmakeを使う（buildツリーがCMake 4.2構成のため。[メモ参照](../../.claude/projects/.../memory)）:
  `& "C:\Users\user\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" --build build --config Debug`
- 手順4完了時: テクスチャ生成で検証レイヤーのエラー（レイアウト遷移・アクセスマスク不整合のVUID）が出ないこと
- 手順6完了時: 立方体の各面にテクスチャが正しく貼られ、回転しても歪まない・遮蔽関係（深度）も維持されること
- リサイズ・最小化復帰・フルスクリーン切替（F11）・Alt+Tab復帰で、テクスチャ描画が継続し検証エラーが出ないこと
- `ctest` 全パス（ECS非変更）

## 追加実装（Phase 8.1）: アルファブレンディング（半透明合成）
PNGのアルファで、透過部分を固定色で塗る（不透明）のではなく、背景や後ろのオブジェクトに対して
実際に透けさせる。カラーブレンドを有効化する。

### 1. パイプライン（[graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp) の `color_blend_attachment`）
標準的なオーバー合成（`out = src.rgb * src.a + dst.rgb * (1 - src.a)`）を設定する:
- `blendEnable = VK_TRUE`
- `srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA`
- `dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`
- `colorBlendOp = VK_BLEND_OP_ADD`
- `srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE`
- `dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO`
- `alphaBlendOp = VK_BLEND_OP_ADD`
- `colorWriteMask` = RGBA（既存のまま）

### 2. フラグメントシェーダー（[triangle.frag](../../shaders/triangle.frag)）
- ブレンドを効かせるには `out_color` のアルファに**実際の値**を出す必要がある。
- 現状の `vec4(mix(base_color, tex.rgb, tex.a), 1.0)` は**アルファを1.0固定＝常に不透明**なので、
  ブレンドを有効化しても効かない。透けさせるなら `out_color = texture(tex_sampler, frag_uv);` に戻す。
- **`base_color`で塗る（不透明）方式と、実際に透ける（ブレンド）方式は目的が排他**。両立はしない。

### 3. 深度書き込みと描画順（重要な制約・学習ポイント）
- `depthWriteEnable = VK_TRUE` のまま半透明を描くと、**透明フラグメントも深度を書き込む**ため、
  後から描く「背後のオブジェクト」が深度テストで棄却され、背後が正しく見えない（クリア色が透ける）。
- 正しい半透明の定石: (a) 不透明を先に描画 → (b) 半透明を**後ろから前へソート**し `depthWrite=FALSE` で描画。
- 現状は全キューブが単一パイプライン・ECS反復順（未ソート）なので、視点角度によってアーティファクトが出る。
  **本フェーズは「有効化して挙動を体験する」までとし、ソート・深度制御・パス分離は将来課題**とする。

### 検証
- 透過部分の向こうに背景（0.2グレー）や後ろの面が透けて見えること。
- 回転させると重なり順で見え方が変わる（未ソートによる既知の限界）こと。
- 検証レイヤーのエラーが出ないこと。

## 将来課題（このフェーズではやらない）
- 半透明の描画順ソート（back-to-front）・不透明/半透明パスの分離・OIT（順序独立透明）
- ミップマップ生成（`vkCmdBlitImage`で実行時生成。異方性と合わせて品質向上）
- ステージングバッファによる頂点/インデックスバッファのDEVICE_LOCAL化（`StagingBuffer`が土台）
- 専用トランスファーキュー（キューファミリ跨ぎの所有権移譲）
- 複数テクスチャ・マテリアル（separate sampled image + sampler、テクスチャ配列、bindless）
- Meshコンポーネント化（エンティティごとに別メッシュ/テクスチャ）
- `find_memory_type`同様、single-time-commandヘルパの共通化（`Texture`以外でも使うようになったら切り出す）
