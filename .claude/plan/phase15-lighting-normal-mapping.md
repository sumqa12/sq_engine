# C++ ゲームエンジン（学習目的）— Phase 15: ライティング（Blinn-Phong → PBR）+ 法線マップ・追加テクスチャスロット

## Context

Phase 14 で**データ構造とアセット管理**が一通り整った（[phase14-material-buffer-hierarchy-gltf.md](phase14-material-buffer-hierarchy-gltf.md) 参照）。マテリアル SSBO・Transform 階層・世代付きハンドル・glTF ローダーが入り、`Duck.glb` / `Box.glb` がノード階層ごとシーンに展開できている。

一方で**絵は phase8（テクスチャマッピング）から本質的に変わっていない**。現在のフラグメントシェーダは実質1行しかない:

```glsl
// shaders/triangle.frag
out_color = texture(textures[nonuniformEXT(m.albedo_index)], frag_uv) * m.base_color;
```

phase14 で「器」だけを用意して**一度も使っていないもの**が、そのまま Phase 15 の材料になる:

| 用意済みの器 | 置き場所 | 現状 |
|---|---|---|
| `Vertex::normal` | [mesh.hpp](../../engine/include/sq/graphics/mesh.hpp)（phase14 D-7 で `color` から置換） | vert が frag へ渡すだけ。**frag が読んでいない** |
| `MaterialData::metallic` / `roughness` | [material_data.hpp](../../engine/include/sq/graphics/material_data.hpp) | glTF から値は入るが**誰も使わない** |
| `MaterialData::emissive` | 同上 | 同上 |
| `MaterialData::alpha_cutoff` | 同上 | 同上（MASK モード未実装） |
| `WorldTransform` の階層 | [hierarchy.hpp](../../engine/include/sq/scene/hierarchy.hpp) | メッシュにしか使っていない。**ライトにも効く**（D-2） |

さらに、ライティングを入れる前に**土台の穴**が2つある:

- **ライトを GPU へ渡す口が一つも無い**。`scene::CameraUBO` は `mat4` 1本きり（[camera.hpp](../../engine/include/sq/scene/camera.hpp)）、プッシュ定数は 0 バイト（[graphics_pipeline.cpp](../../engine/src/graphics/graphics_pipeline.cpp)）。
- **色空間が破綻している**。[texture.cpp](../../engine/src/graphics/texture.cpp) は `VK_FORMAT_R8G8B8A8_SRGB` 決め打ちでサンプル結果は linear、一方 [swapchain.cpp](../../engine/src/graphics/swapchain.cpp) は `B8G8R8A8_UNORM` を**優先して選んでいる**。linear 値を UNORM サーフェスへ生で書いているので、**今の画面は既に元画像より暗い**。unlit だと「そういう絵」で済んでいたが、ライト計算は乗算なのでこの誤差がそのまま増幅される。

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-0. 着手順は ⓪ → ① → ② → ③

| 順 | 項目 | 理由 |
|---|---|---|
| 0 | **⓪ 色空間（ガンマ）の整備** | 先に直さないと「暗い」の原因が**ライトの式なのかガンマなのか切り分けられない**。変更量は小さい |
| 1 | **① Blinn-Phong** | ライト配列の流し込み・法線行列・ECS ライトという**配管**を、式が単純なうちに通す |
| 2 | **② Cook-Torrance PBR** | ①の配管の上で**式だけ**を差し替える。絵が破綻したら疑うのは BRDF の式だけになる |
| 3 | **③ 法線マップ + 追加テクスチャスロット** | 接空間・sRGB/UNORM 分離・glTF 拡張。**ここが本フェーズの地雷原** |

**① を挟む理由**（いきなり PBR を書かない理由）: PBR で絵が想定と違ったとき、原因の候補が「法線の向き」「接空間」「色空間」「BRDF の式」「トーンマップ」と5つ同時に立つ。Blinn-Phong を一度通しておけば、②で絵が壊れたときに疑うのは BRDF の式だけになる。**①は捨てる前提の足場**（D-5）。

### D-1. 色空間はスワップチェーンを `*_SRGB` にして解決する

- 現状の経路: テクスチャ（SRGB フォーマット）→ **サンプル時にハードウェアが linear へデコード** → シェーダで乗算 → **UNORM サーフェスへ生のまま書く** → ディスプレイは sRGB として解釈する。つまり**エンコードが抜けている**。
- 対処は2択。
  - (a) スワップチェーンを `B8G8R8A8_SRGB` にして、**書き込み時にハードウェアがエンコード**する
  - (b) フラグメントの末尾で `pow(color, vec3(1.0/2.2))` を自前で掛ける
- **(a) を採る**。理由は3つ:
  1. **半透明ブレンドが linear 空間で行われる**。(b) だとブレンダには既にエンコード済みの値が渡るので、phase11 で作った半透明パスの合成結果が微妙に狂う
  2. シェーダに手を入れない（②③でシェーダを何度も書き換えるので、色空間の責務を外に出しておきたい）
  3. 将来ポストプロセス（トーンマップ以降、ブルーム等）を入れるとき、「最終出力だけがエンコード」という構造が素直
- ★ **クリア色は linear 値として扱われるようになる**ので、今までと同じ灰色にしたければ値を変える必要がある（後述 ⓪-2）。

### D-2. ライトは ECS コンポーネント、位置と向きは `WorldTransform` から取る

- `scene::Light` に**座標も方向も持たせない**。
  - 位置 = `world.matrix[3]` の xyz
  - 向き = `-world.matrix[2]` の xyz（**-Z 前方**。glTF・OpenGL 系の慣習に合わせる）
- こうすると phase14 ④ で作った階層が**そのままライトに効く**。親エンティティに付ければライトが追従し、「キャラクタが持つ松明」「車のヘッドライト」が追加コード無しで書ける。
- ★ **ライトエンティティにも生成時に `WorldTransform` を必ず付ける**（phase14 D-4 の規約）。`TransformSystem::update` は `<Transform, WorldTransform>` を回すので、付け忘れたライトは**原点に居ることになる**（クラッシュしないので気付きにくい）。

### D-3. ライトバッファは set=0 の binding=2（`kFramesInFlight` 複製の SSBO）

phase14 D-2 で決めた set の役割分担に素直に従う。

| set | 性格 | 中身 | 個数 |
|---|---|---|---|
| **set=0** | **フレームごとに変わる** | binding=0: カメラUBO<br>binding=1: インスタンス SSBO<br>binding=2: **ライト SSBO**（← 今回追加） | `kFramesInFlight` 個 |
| **set=1** | **起動後は不変（アセット）** | binding=0: bindless テクスチャ配列<br>binding=1: マテリアル SSBO | 全体で1個 |

- ライトは毎フレーム動きうる（太陽が回る・キャラに付いた光源が動く）ので set=0。[InstanceBuffer](../../engine/include/sq/graphics/instance_buffer.hpp) と**完全に同型**にでき、収集フェーズの流れにそのまま乗る。
- **ライト数は SSBO の先頭要素に混ぜない**。`lights[0]` だけ意味が違う形は読みづらく、`gl_InstanceIndex` 相当の添字計算も1ずれる。**`CameraUBO` に `light_count` を足す**（①-4）。
- ★ binding=2 のステージは **`VK_SHADER_STAGE_FRAGMENT_BIT`**。binding=0（カメラ）と binding=1（インスタンス）は `VERTEX_BIT` なので、ここだけ違う。

### D-4. `LightData` は `vec4` 3本（48 バイト）で持つ

```
vec4 position_type    // xyz = ワールド位置,  w = 種別（0=Directional, 1=Point）
vec4 direction_range  // xyz = ワールド方向,  w = 有効半径
vec4 color_intensity  // rgb = 色（linear）,  a = 強度
```

- std430 では `vec3` も **16 バイト境界に揃う**ので、`vec3 + float` を並べると結局 `vec4` 2本ぶんの領域を食う。だったら**最初から `vec4` に詰めて、余った成分に意味を持たせる**方が、パディングを数える作業が消えてレイアウト事故も起きない。`MaterialData`（phase14 ①）で `emissive.w` を空けてあるのと同じ考え方。
- ★ 種別を `float` の `w` に入れて `int(w)` で読む形にする。`uint` メンバを別に置くと 16 バイトの詰め直しが発生する。

### D-5. Blinn-Phong は「捨てる前提の足場」

- ② で Cook-Torrance に差し替える。`roughness → 鏡面指数` の暫定変換（`exp2(11 * (1 - roughness))` 等）も一緒に捨てる。
- **凝らないこと**。①の目的は「ライト配列が届いているか」「法線が正しい向きか」を目で確かめられる状態を作ることだけ。

### D-6. テクスチャのフォーマットは**用途**で決める（sRGB / UNORM）

| 用途 | フォーマット | 理由 |
|---|---|---|
| baseColor / emissive | **`R8G8B8A8_SRGB`** | 「色」なので sRGB で保存されている。linear へデコードして使う |
| normal / metallicRoughness / occlusion | **`R8G8B8A8_UNORM`** | 「数値」であって色ではない。デコードすると値が変わってしまう |

- ★ **間違えても落ちない**のが厄介。法線マップを sRGB で読むと「陰影の向きが微妙にずれた、それらしい絵」になり、roughness を sRGB で読むと「全体的にツルツル寄り」になる。**バリデーションも警告を出さない**。
- 現状 [texture.cpp](../../engine/src/graphics/texture.cpp) は SRGB が3箇所にハードコードされている（mip の blit 対応判定 / `VkImageCreateInfo::format` / `VkImageViewCreateInfo::format`）。`Texture` の2つのコンストラクタ → `create_from_pixels` → `TextureRegistry::load` / `load_from_pixels` / `register_texture` まで**フォーマットを引数で通す**（③-1）。
- ★ `TextureRegistry::by_path_` のキーが**パスのみ**なので、同じ画像ファイルを sRGB と UNORM の両方で使うと**キャッシュが衝突する**。キーを `{path, format}` にする。

### D-7. 接空間は頂点属性（TANGENT）で持つ

- `Vertex` に `glm::vec4 tangent` を追加する（`xyz` = 接線、`w` = 従法線の符号 ±1）。glTF の `TANGENT` アクセサと同じ形。
- **採らない案**: フラグメントで `dFdx/dFdy` から接空間を導出する（Mikkelsen 法）。頂点属性が増えず glTF の TANGENT 有無も気にしなくてよいが、
  - ピクセル単位の微分なのでポリゴン境界で品質が落ちる
  - 「接空間とは何か・なぜ w が要るのか」を扱うのが本フェーズの学習目的なので、隠してしまうと意味が薄い
- glTF が `TANGENT` を持たない場合は**ローダー側で UV から生成**する（③-4）。

### D-8. 環境光は定数で妥協する（IBL は phase16）

- `ambient = kAmbient * albedo * ao`（`kAmbient = 0.03` 程度）。
- ★ **PBR で metallic=1 のマテリアルはほぼ真っ黒になる**。金属は拡散反射を持たず、鏡面反射は「周囲の映り込み」＝ IBL でしか得られないため。**これはバグではなく IBL 未実装の帰結**。②の検証でマテリアルボールを並べると必ず遭遇するので、先に知っておくこと。
- ★ 逃げ道として「定数の環境色を鏡面側にも薄く足す」ことはできるが、物理的な裏付けは無い。やるなら「IBL までの暫定」とコメントを残す。

---

## ⓪ 色空間とトーンマップの整備

### ⓪-1. スワップチェーンを sRGB フォーマットにする（[swapchain.cpp](../../engine/src/graphics/swapchain.cpp)）

```cpp
// Swapchain::create のサーフェス形式選択:
//   現在: colorSpace == SRGB_NONLINEAR かつ format が B8G8R8A8_UNORM / R8G8B8A8_UNORM を優先
//   変更: 同じ colorSpace のまま、format を B8G8R8A8_SRGB / R8G8B8A8_SRGB の優先へ変える（D-1）
//
// ★ SRGB 形式が1つも見つからなかった場合は formats[0] にフォールバックする現在の構造を残すこと。
//   その環境では「暗い絵」に戻るが、落ちるよりはよい。
//   （フォールバックしたことが分かるよう spdlog::warn を出しておく）
```

- ★ [RenderPass](../../engine/src/graphics/render_pass.cpp) のカラーアタッチメント形式は `swapchain_->image_format()` を受け取る作りになっている（[renderer.cpp](../../engine/src/graphics/renderer.cpp) の構築箇所）ので、**追加の修正は要らない**。ただし RenderPass は起動時に1回しか作っていないため、再生成でフォーマットが変わる環境があると不整合になる。**実際に変わることは無い想定**だが、`recreate_swapchain` で形式が変化していないか `assert` を置いておくと安全。
- ★ フルスクリーン排他（F11）の取得・喪失で再生成が走る経路でも同じフォーマットが選ばれることを確認する。

### ⓪-2. クリア色を linear 値に直す（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）

```cpp
// 現在: clear_values[0].color = {{0.8f, 0.8f, 0.8f, 1.0f}}
// これは「sRGB の 0.8」のつもりで書かれた値。SRGB スワップチェーンでは
// 書き込み時にエンコードされるため、そのままだと明るい灰色になる。
//
// 今までと同じ見た目にしたいなら linear へ直す:  pow(0.8, 2.2) ≒ 0.60
// ★ ここで「暗くなった / 明るくなった」に気付けるのが、⓪を最初にやる効用でもある。
```

### ⓪-3. linear で計算し、最後にトーンマップする約束を決める

```glsl
// triangle.frag の出力の作法（②で実際に必要になる。ここで決めておく）:
//   1. テクスチャのサンプル結果は既に linear（ハードウェアがデコード済み）
//   2. 照明計算はすべて linear で行う
//   3. 最後にトーンマップ（Reinhard か ACES 近似）で [0,1] に収める
//   4. sRGB エンコードは**スワップチェーン任せ**（シェーダでは pow を掛けない。D-1）
//
// ★ glTF の baseColorFactor / emissiveFactor は仕様上すでに linear。
//   「テクスチャだけが sRGB」で、factor には何もしない。二重デコード注意。
```

### ⓪-4. 検証
- 絵の明るさが変わること自体は正しい。**元画像（textures/*.png）をビューアで開いた色と、画面上の色が一致する**ことを確認する（今までは画面の方が暗かったはず）。
- 半透明パス（`main.cpp` の a=0.5 マテリアル）の合成結果が破綻していないこと。
- F11・Alt+Tab・リサイズでフォーマット関連のバリデーションエラーが出ないこと。

---

## ① ライティング基盤（Blinn-Phong）

### ①-1. ライトのコンポーネント（新規 `light.hpp`）

```cpp
// engine/include/sq/scene/light.hpp（新規）
#pragma once

#include <glm/glm.hpp>

namespace sq::scene {

// ライトの種別（phase15 ①）。
// ★ スポットライトは今回入れない（円錐角と減衰で項目が2つ増え、
//   シャドウマップ（phase16）と一緒に入れた方が確認しやすいため）。
enum class LightType {
    Directional,  // 平行光源（太陽）。位置を持たず、向きだけを使う
    Point,        // 点光源。位置と減衰半径を使う
};

// 光源のECSコンポーネント（phase15 ① / D-2）。
//
// ★ 位置も方向も持たない。位置は WorldTransform::matrix[3]、
//   方向は -WorldTransform::matrix[2]（-Z 前方）から取る。
//   こうすると phase14 ④ の親子階層がそのままライトに効く
//   （親に付ければ追従する）。
//
// ★ ライトエンティティにも生成時に Transform と WorldTransform を必ず付けること。
//   付け忘れると TransformSystem の対象から外れ、黙って原点のライトになる。
struct Light {
    LightType type = LightType::Directional;

    glm::vec3 color{1.0f};   // 光の色（linear。⓪-3 の約束どおり sRGB 値を入れない）
    float intensity = 1.0f;  // 強度。0 以下なら収集時に捨てる

    // Point のみ使う。ここを超えると寄与を 0 にする（減衰の打ち切り）。
    // ★ 物理的には 1/d² は無限に届くが、打ち切らないと全ライトが全ピクセルに効いて重くなる。
    float range = 10.0f;
};

}  // namespace sq::scene
```

### ①-2. GPU 側のライトデータ（新規 `light_data.hpp`）

```cpp
// engine/include/sq/graphics/light_data.hpp（新規）
#pragma once

#include <glm/glm.hpp>

namespace sq::graphics {

// ライト1件のGPU側レイアウト（phase15 ① / D-4）。SSBO の配列要素になる。
//
// なぜ vec4 3本なのか:
//   std430 では vec3 も 16 バイト境界に揃うため、`vec3 + float` を並べても
//   結局 vec4 2本ぶんの領域を使う。ならば最初から vec4 に詰めて、
//   余る成分（w）に意味を持たせた方が、パディングを数える作業自体が消える。
//   MaterialData で emissive.w を将来用に空けてあるのと同じ考え方。
//
// ★ シェーダの std430 ブロックと完全に一致させること。
struct LightData {
    glm::vec4 position_type{0.0f};    // offset  0, 16  xyz=ワールド位置 / w=種別(0=Dir, 1=Point)
    glm::vec4 direction_range{0.0f};  // offset 16, 16  xyz=ワールド方向(正規化) / w=有効半径
    glm::vec4 color_intensity{1.0f};  // offset 32, 16  rgb=色(linear) / a=強度
};

static_assert(sizeof(LightData) == 48, "std430 のレイアウトと一致させること");
static_assert(offsetof(LightData, color_intensity) == 32, "シェーダ側のメンバ順と一致させること");

}  // namespace sq::graphics
```

### ①-3. ライトバッファ（新規 `light_buffer.hpp` / `.cpp`）

```cpp
// engine/include/sq/graphics/light_buffer.hpp（新規）
//
// [InstanceBuffer](../../engine/include/sq/graphics/instance_buffer.hpp) と**同型**。
// 永続マップした SSBO に、毎フレーム先頭から count 件を memcpy するだけ。
// ★ kFramesInFlight 個作ること（GPU が前フレームを読んでいる最中に上書きしないため）。
//
// class LightBuffer : public Buffer {
// public:
//     LightBuffer(GpuAllocator& allocator, VkDevice device, std::size_t max_lights);
//     ~LightBuffer();   // 永続マップの unmap のみ（解放は基底デストラクタ）
//
//     // data の先頭 count 要素を書き込む。★ capacity_ を超える書き込みは弾くこと。
//     void update(const LightData* data, std::size_t count);
//     [[nodiscard]] std::size_t capacity() const;
// private:
//     void* mapped_ = nullptr;
//     std::size_t capacity_ = 0;
// };
//
// TODO: InstanceBuffer とコードが完全に重複する。テンプレート化（MappedStorageBuffer<T>）は
//   3本目（phase16 で骨行列バッファ等が来たとき）に検討する。2本で括ると早すぎる。
```

- `kMaxLights = 64`（Renderer 側の定数）。48 バイト × 64 = 3KiB なので、多めに取っても実害は無い。

### ①-4. `CameraUBO` の拡張（[camera.hpp](../../engine/include/sq/scene/camera.hpp)）

```cpp
// Uniform Bufferへ転送するカメラデータのGPU側レイアウト。
// phase15 ①: スペキュラ計算に視点位置が要るため camera_position を、
//   ライト配列の有効件数を渡すため light_count を追加した。
//
// ★ UBO は **std140**（SSBO の std430 とは規則が違う）。
//   std140 では配列要素やスカラーの詰め方が緩く、「C++ 側で詰めたつもり」が
//   シェーダ側とずれやすい。**スカラーを裸で置かず vec4 / uvec4 に揃える**のが安全策。
struct CameraUBO {
    glm::mat4  view_projection;   // offset  0, 64
    glm::vec4  camera_position;   // offset 64, 16  xyz=ワールド位置, w=未使用
    glm::uvec4 light_count;       // offset 80, 16  x=有効ライト数, yzw=未使用
};

static_assert(sizeof(CameraUBO) == 96, "std140 のレイアウトと一致させること");
```

> ★ 値の出所は既にある。[renderer.cpp](../../engine/src/graphics/renderer.cpp) の収集フェーズが半透明ソート用に `cam_pos` を解決済みなので、UBO へ代入を1行足すだけでよい。

### ①-5. Renderer の変更点（すべて既存関数の中）

```cpp
// 1) create_descriptor_set_layout: set=0 に binding=2 を追加する
//      descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
//      descriptorCount = 1
//      stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT   // ★ binding=0/1 は VERTEX。ここだけ違う
//    ★ bindingCount を 2 → 3 に直すこと（pBindings の要素数と一致していないと即バリデーション違反）。
//
// 2) create_descriptor_pool: STORAGE_BUFFER の枠を増やす
//      現在: kFramesInFlight + 1 = 3（インスタンス×2 + マテリアル×1）
//      変更: kFramesInFlight * 2 + 1 = 5（+ ライト×2）
//    ★ maxSets は変わらない（セットの数は増えていない。binding が増えただけ）。
//
// 3) create_light_buffers()（新規。create_instance_buffers と同じ形）
//      kFramesInFlight 個の LightBuffer を作る。
//
// 4) create_descriptor_sets: vkUpdateDescriptorSets を 2 件 → 3 件へ
//      追加分: dstBinding = 2, STORAGE_BUFFER, light_buffers_[i] の全域（VK_WHOLE_SIZE）
//
// 5) draw_frame の収集フェーズ: ライトの収集を足す
//      lights_.clear();
//      registry.view<scene::Light, scene::WorldTransform>().each(
//          [&](ecs::Entity, scene::Light& light, scene::WorldTransform& wt) {
//              // ★ intensity <= 0 は詰めない（GPU 側で無駄に回さない）
//              // 位置 = glm::vec3(wt.matrix[3])
//              // 方向 = glm::normalize(-glm::vec3(wt.matrix[2]))   // -Z 前方（D-2）
//              //   ★ 親に非等方スケールが掛かっていると基底の長さが 1 でない。必ず正規化する。
//              // LightData を組んで lights_ へ push_back
//          });
//      // ★ kMaxLights を超えたら警告してクランプ（instances_ と同じ扱い）
//      light_buffers_[current_frame_]->update(lights_.data(), lights_.size());
//      camera_ubo.light_count = glm::uvec4(lights_.size(), 0, 0, 0);
//
//    ★ カメラUBOの更新は「ライト収集の後」に移すこと。
//      light_count を埋めてから転送しないと、1フレーム古い件数を送ることになる。
//
// 6) メンバ追加:
//      static constexpr std::uint32_t kMaxLights = 64;
//      std::vector<std::unique_ptr<LightBuffer>> light_buffers_;
//      std::vector<LightData> lights_;   // 組み立て用（毎フレームのヒープ確保を避ける）
```

### ①-6. シェーダ（[triangle.vert](../../shaders/triangle.vert) / [triangle.frag](../../shaders/triangle.frag)）

```glsl
// ---- triangle.vert ----
// 追加の出力: ワールド座標（点光の距離計算と視線ベクトルに要る）
layout(location = 3) out vec3 frag_world_pos;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = camera.view_proj * world_pos;
    frag_world_pos = world_pos.xyz;

    // ★ 法線はワールドへ「逆転置」で変換する。
    //   mat3(model) をそのまま掛けると、非等方スケール（親のスケール含む）が入ったとき
    //   法線が面に対して傾き、陰影が歪む。
    mat3 normal_matrix = transpose(inverse(mat3(inst.model)));
    frag_normal = normalize(normal_matrix * in_normal);
    // ★ inverse() は頂点ごとに走るので本来は重い。
    //   CPU 側で計算して InstanceData に積む案は phase16 の課題（後述）。
}

// ---- triangle.frag ----
struct LightData {
    vec4 position_type;
    vec4 direction_range;
    vec4 color_intensity;
};
layout(std430, set = 0, binding = 2) readonly buffer LightBuffer { LightData lights[]; };

// 実装の骨子（D-5: 捨てる前提なので凝らない）:
//   vec3 N = normalize(frag_normal);
//   vec3 V = normalize(camera.camera_position.xyz - frag_world_pos);
//   vec3 result = kAmbient * albedo;
//   for (uint i = 0; i < camera.light_count.x; ++i) {
//       // 種別で L と減衰を分ける
//       //   Directional: L = -direction, attenuation = 1
//       //   Point:       L = normalize(pos - frag_world_pos)
//       //                d = length(pos - frag_world_pos)
//       //                attenuation = 1/(d*d) * smoothstep で range で 0 に落とす
//       //   ★ d が 0 に近いと 1/(d*d) が爆発する。max(d*d, 0.0001) でクランプすること。
//       // diffuse  = max(dot(N, L), 0.0)
//       // specular = pow(max(dot(N, normalize(L + V)), 0.0), shininess)
//       //   shininess は roughness からの暫定変換（②で捨てる）
//       // result += (diffuse * albedo + specular) * color * intensity * attenuation;
//   }
//   out_color = vec4(result, base_color.a * albedo_tex.a);
```

> ★ **新しいシェーダファイルを増やす場合**は [sandbox_graphics/CMakeLists.txt](../../sandbox_graphics/CMakeLists.txt) のシェーダ収集が `file(GLOB ...)` なので、**CMake の再生成が必要**（追加しただけではビルドに載らない）。今回は `triangle.vert` / `triangle.frag` を書き換えるだけなので、この問題は踏まない。

### ①-7. 検証シーン（[main.cpp](../../sandbox_graphics/main.cpp)）

```cpp
// 方向光1つ + 点光を数個置く。
// ★ Transform / WorldTransform / Light の3点セットで生成すること（D-2）。
//
//   ecs::Entity sun = registry.create();
//   registry.add(sun, scene::Transform{ pos, glm::quat(...), glm::vec3(1.0f) });
//   registry.add(sun, scene::WorldTransform{});
//   registry.add(sun, scene::Light{ .type = LightType::Directional, .color = {1.0f, 0.95f, 0.9f}, .intensity = 3.0f });
//
// ★ 方向は Transform の回転で決まる（-Z 前方）。「斜め上から差す光」にしたければ
//   glm::quatLookAt(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), up) 等で作る。
//   ここが直感に反しやすいので、まず点光で動作確認してから方向光を合わせるとよい。
//
// TODO: ライトを毎フレーム回す簡単な System を足すと、陰影が動いて確認しやすい
//   （ecs::System は const Registry& を受けるが、Transform の setter は
//    Registry::get<T>() const が T& を返すので呼べる。phase14 D-3 参照）。
```

### ①-8. 検証
- 立方体を回すと面ごとに明暗が変わること（**回しても陰影が一緒に回ってしまうなら法線行列が抜けている**）。
- `frag_normal * 0.5 + 0.5` をそのまま出力するデバッグ経路で、法線の向きが面ごとに正しいこと。
- `Duck.glb` に陰影が付くこと。
- ★ **`Box.glb` が平坦に見えたら NORMAL の欠損を疑う**。現在のローダーは NORMAL が無いプリミティブを `{0,1,0}` で埋めており（[gltf_loader.cpp](../../engine/src/assets/gltf_loader.cpp)）、unlit では見えなかった問題が**ライティングを入れて初めて可視化される**（③-5 で対処）。
- ライトを0個にするとアンビエントだけの真っ暗（一様な暗さ）になること。
- 点光から離れると滑らかに暗くなり、`range` の外で完全に0になること。
- **親子を組んだライト**（例: 回転する親の子にした点光）が親に追従すること（phase14 ④ の成果の再確認）。

---

## ② Cook-Torrance PBR への差し替え

### ②-1. BRDF の実装（[triangle.frag](../../shaders/triangle.frag)）

```glsl
// マイクロファセット BRDF（Cook-Torrance）。①の配管はそのまま、式だけを差し替える。
//
//   f = D * G * F / (4 * NdotV * NdotL)
//
// D: GGX / Trowbridge-Reitz
//   float a = roughness * roughness;        // ★ 知覚的粗さ → 実効粗さ（2乗するのが慣習）
//   float a2 = a * a;
//   float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
//   D = a2 / (PI * d * d);
//
// G: Smith（Schlick-GGX を視線側・光源側の2回）
//   float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;   // 直接光用の k
//   G1(x) = x / (x * (1 - k) + k);
//   G = G1(NdotV) * G1(NdotL);
//
// F: Schlick 近似
//   vec3 F0 = mix(vec3(0.04), albedo, metallic);   // 誘電体は 4%、金属は albedo そのもの
//   F = F0 + (1 - F0) * pow(1 - HdotV, 5);
//
// エネルギー配分:
//   vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);  // ★ 金属は拡散反射を持たない
//   Lo += (kD * albedo / PI + specular) * radiance * NdotL;
//
// ★ ゼロ割りの罠が3箇所ある:
//   1. roughness = 0 で D が発散する      → clamp(roughness, 0.03, 1.0)
//   2. 分母の 4 * NdotV * NdotL が 0      → + 0.0001 を足す
//   3. 点光の 1/(d*d) が d→0 で発散       → ①と同じく max(d*d, 0.0001)
//   どれも「特定の角度でだけ真っ白なピクセルが出る」という形で現れる。
```

### ②-2. emissive・alpha・トーンマップ

```glsl
// 1. emissive は最後に加算する（ライトに依存しない）
//      color = Lo + ambient + m.emissive.rgb;
//
// 2. alpha は base_color.a * texture.a のまま（半透明パスの振り分けは CPU 側。phase14 ①-4）
//
// 3. alpha_cutoff（MASK モード）: ③-5 と対で入れる
//      if (m.alpha_cutoff > 0.0 && alpha < m.alpha_cutoff) { discard; }
//      ★ discard は早期深度テストを無効化するので、不透明パスの性能が落ちる。
//        「MASK のマテリアルにだけ効く」形にしておくこと（cutoff = 0 なら素通り）。
//
// 4. トーンマップ（⓪-3 の約束）
//      Reinhard:  c = c / (c + 1.0)
//      ACES 近似: よりコントラストが出る。式は3行程度
//      ★ exposure（露出）を掛けてから通すと調整しやすい: c *= exposure;
//      ★ sRGB エンコードはここでは**しない**（スワップチェーン任せ。D-1）
```

### ②-3. 検証シーン: マテリアルボール（[main.cpp](../../sandbox_graphics/main.cpp)）

```cpp
// ★ 現在 kGridSide = 0 になっていて、phase13/14 で作った格子シーンが
//   1体も生成されていない（死にコードになっている）。ここを PBR の検証台に作り替える。
//
//   metallic ∈ {0, 1} × roughness ∈ {0.05, 0.2, 0.4, 0.6, 0.8, 1.0} の 12 マテリアルを
//   registry へ登録し、6×2 の格子に立方体（できれば球メッシュ）を並べる。
//   マテリアル選択を「格子座標 → マテリアル」にする形は phase14 ①-5 の仕組みがそのまま使える。
//
// TODO: 球メッシュ（add_sphere_mesh）を MeshRegistry に足すと PBR の確認が段違いにやりやすい
//   （立方体は法線が6方向しか無いので、roughness の差が読み取りにくい）。
//   UV 球なら緯度・経度の2重ループで書ける。法線は正規化した位置そのもの。
```

### ②-4. 検証
- roughness を上げるとハイライトが**広く弱く**なること（下げると小さく鋭く）。
- metallic=1 の列が**ほぼ真っ黒**になること。★ これは D-8 のとおり**正しい挙動**（IBL 未実装の帰結）。ここで「壊れた」と判断して式をいじらないこと。
- 真横から見たときにフレネル（縁が明るくなる）が見えること。
- ①と比べて**破綻が無い**こと（全体が真っ白・真っ黒・特定角度だけ白飛び、はゼロ割りの兆候。②-1 の3箇所を疑う）。
- 半透明マテリアルが引き続き正しい順序で合成されること。

---

## ③ 法線マップ + 追加テクスチャスロット

### ③-1. `Texture` のフォーマット引数化（D-6。**本フェーズ最大の地雷**）

```cpp
// engine/include/sq/graphics/texture.hpp
//
// 2つのコンストラクタと create_from_pixels に VkFormat を追加する:
//
//   Texture(VkPhysicalDevice, VkDevice, GpuAllocator&,
//           std::uint32_t graphics_queue_family, VkQueue graphics_queue,
//           const std::string& path, VkFormat format);
//
//   Texture(..., const unsigned char* pixels, std::uint32_t width, std::uint32_t height,
//           VkFormat format);
//
//   void create_from_pixels(..., VkFormat format);
//
// ★ 既定引数（= VK_FORMAT_R8G8B8A8_SRGB）は付けないこと。
//   付けると「書き忘れ」が sRGB として黙って通り、法線マップの取り違えを
//   コンパイラが検出できなくなる。**全呼び出し箇所に明示させる**のが目的。
//
// ★ 直す箇所は texture.cpp の3つ（どれか1つでも残すと不整合）:
//   1. supports_linear_blit(..., format) の判定  ← mip 生成可否がフォーマットで変わる
//   2. VkImageCreateInfo::format
//   3. VkImageViewCreateInfo::format
```

```cpp
// engine/include/sq/graphics/texture_registry.hpp
//
//   scene::TextureId load(const std::string& path, VkFormat format);
//   scene::TextureId load_from_pixels(const unsigned char* pixels,
//                                     std::uint32_t width, std::uint32_t height,
//                                     VkFormat format);
//
// ★ by_path_ のキーを {path, format} にする（D-6）。
//   同じ画像を baseColor（sRGB）と occlusion（UNORM）の両方で使うモデルは実在する。
//   キーがパスだけだと、後から要求された方が**静かに前のフォーマットで返される**。
//   実装は std::map<std::pair<std::string, VkFormat>, TextureId> でも、
//   path + "#" + std::to_string(format) の連結キーでもよい。
```

### ③-2. フラット法線テクスチャ（中立フォールバック）

```cpp
// TextureRegistry に追加:
//   scene::TextureId create_flat_normal_texture();   // 1×1 の (128,128,255,255)
//
// 実装は create_white_texture と同型（contains() による二重登録防止も同じ）。
// ★ ただしフォーマットは **UNORM**。
//   白（255,255,255）は sRGB でも UNORM でも 1.0 なので差が出ないが、
//   128 は sRGB で読むと 0.5 ではなく約 0.22 になる。つまり sRGB で読んだ瞬間、
//   「真上を向いた法線」のつもりが**斜めを向いた法線**になり、
//   それでも絵は出る（陰影がわずかにおかしいだけ）。最悪の壊れ方なので注意。
//
// ★ metallicRoughness と occlusion に専用の中立テクスチャは要らない。
//   glTF 仕様ではどちらも factor に**乗算**されるので、白（=1.0）が恒等元になる。
//   既存の white_texture() をそのまま使う。
//
// ★ 呼ぶ順序: default.png の load → create_white_texture → create_flat_normal_texture。
//   default_texture_ は「最初に登録されたもの」で決まるため（phase14 ③）。
```

### ③-3. `MaterialData` の拡張（48 → 64 バイト）

```cpp
struct MaterialData {
    glm::vec4 base_color{1.0f};                    // offset  0, 16
    glm::vec4 emissive{0.0f};                      // offset 16, 16
    float metallic = 0.0f;                         // offset 32,  4
    float roughness = 1.0f;                        // offset 36,  4
    float alpha_cutoff = 0.0f;                     // offset 40,  4  ★ 既定を 0.5 → 0.0 に変える（③-5）
    std::uint32_t albedo_index = 0;                // offset 44,  4
    std::uint32_t normal_index = 0;                // offset 48,  4  ← 追加
    std::uint32_t metallic_roughness_index = 0;    // offset 52,  4  ← 追加
    std::uint32_t occlusion_index = 0;             // offset 56,  4  ← 追加
    std::uint32_t emissive_index = 0;              // offset 60,  4  ← 追加
};
static_assert(sizeof(MaterialData) == 64, "std430 のレイアウトと一致させること");
```

> - uint 4本でちょうど 16 バイト増え、64 バイト（16の倍数）を保てる。
> - ★ [triangle.frag](../../shaders/triangle.frag) の `struct MaterialData` も**同じ順序で**直すこと。片方だけ直すと全マテリアルの見た目が崩れる（phase14 ① で同じ注意をしている）。
> - ★ `alpha_cutoff` の既定値を **0.5 → 0.0** に変える。②-2 で「0 なら MASK 無効」という約束にしたため、0.5 のままだと全マテリアルが MASK 扱いになって半透明が消える。

```cpp
// engine/include/sq/scene/material.hpp または material_registry.hpp
//
// ★ MaterialRegistry::add(const MaterialData&, scene::TextureId albedo) の引数が破綻する。
//   テクスチャが5枚になったので、まとめて渡す型を作る:
//
//   struct MaterialTextures {
//       TextureId albedo{};
//       TextureId normal{};
//       TextureId metallic_roughness{};
//       TextureId occlusion{};
//       TextureId emissive{};
//   };
//
//   scene::MaterialId add(const MaterialData& data, const MaterialTextures& textures);
//
// ★ フォールバック先が用途ごとに違う（ここが add の中身の要点）:
//     albedo             → white_texture()        （乗算の恒等元）
//     normal             → flat_normal_texture()  （真上を向いた法線）
//     metallic_roughness → white_texture()        （factor をそのまま通す）
//     occlusion          → white_texture()        （遮蔽なし）
//     emissive           → white_texture()        （emissive factor をそのまま通す）
//   ★ 「無効なハンドル」と「未登録のハンドル」はどちらも default_texture()（市松）にしない。
//     テクスチャを持たないのは**正常**であって、異常通知の市松を出す場面ではない
//     （phase14 ③ の white/default の使い分けと同じ話）。
//
// ★ MaterialRegistry::update は add と違ってフォールバック解決を通っていない。
//   解決処理を private 関数（resolve_textures）へ括り出して**両方から呼ぶ**こと。
//   phase14 で Texture::create_from_pixels を括り出したのと同じ理由で、
//   片方だけ直す事故が起きる形を残さない。
```

### ③-4. TANGENT の追加（D-7）

```cpp
// engine/include/sq/graphics/mesh.hpp
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;  // ← 追加。xyz=接線, w=従法線の符号(±1)。glTF の TANGENT と同じ形
    glm::vec2 uv;
};

// ★ なぜ w が要るのか:
//   従法線は B = cross(N, T) * w で求める。UV がミラーリングされている面では
//   従法線の向きが反転するため、その1ビットを符号として持ち運ぶ必要がある。
//   （ここを常に +1 にすると、左右対称なモデルの片側だけ凹凸が反転する）
```

```cpp
// engine/src/graphics/graphics_pipeline.cpp
//   attribute description に location=3（R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)）を追加。
//   ★ uv の location が 2 のままだと tangent と衝突する。**uv を 3、tangent を 2** にするか、
//     tangent を 3 にして uv を 2 に留めるか、どちらでもよいが
//     **シェーダ側の layout(location=...) と必ず揃える**こと。
//   ★ vertexAttributeDescriptionCount を 3 → 4 に直す（配列だけ増やして数を忘れる事故が定番）。
//
// engine/src/graphics/mesh_registry.cpp
//   add_cube_mesh / add_plane_mesh の頂点データに接線を入れる。
//   立方体は面ごとに接線が決まる（+X 面なら T=(0,0,-1) 等）。UV の張り方と整合させること。
```

```cpp
// glTF に TANGENT が無い場合の生成（gltf_loader.cpp）:
//
//   1. 頂点ごとの累積用 vector<glm::vec3> tan_accum(vertex_count, 0) を用意
//   2. インデックスを3つずつ辿り、三角形ごとに
//        edge1 = p1 - p0, edge2 = p2 - p0
//        duv1  = uv1 - uv0, duv2 = uv2 - uv0
//        det   = duv1.x * duv2.y - duv2.x * duv1.y
//        ★ |det| が極小（UV が縮退した面）なら **スキップ**する。
//          ここを弾かないと 1/det が発散し、NaN が頂点経由で周囲に伝播して
//          「モデルの一部が消える」形で出る（原因が非常に追いにくい）。
//        T = (edge1 * duv2.y - edge2 * duv1.y) / det
//      を3頂点すべてに累積する
//   3. 頂点ごとに、法線に対してグラム・シュミット直交化して正規化:
//        T = normalize(T - N * dot(N, T));
//      w は cross(N, T) と累積した従法線の向きを比べて ±1 を決める
//        （簡易には w = 1.0 固定でも多くのモデルは正しく見えるが、
//          NormalTangentTest.gltf のような検証モデルでは差が出る）
//   4. 法線がゼロ（縮退）の頂点は T = (1,0,0,1) で埋めて落ちないようにする
```

### ③-5. glTF ローダーの拡張（[gltf_loader.cpp](../../engine/src/assets/gltf_loader.cpp)）

```cpp
// (A) 用途を先に決めてから画像を読む【重要】
//   現在 load_images は model.images を**用途に関係なく一律 sRGB で**登録している。
//   手順を変える:
//     1. model.materials を先に走査し、「image index → 用途（sRGB か UNORM か）」の表を作る
//          baseColorTexture / emissiveTexture      → sRGB
//          normalTexture / metallicRoughnessTexture / occlusionTexture → UNORM
//     2. 表に従って load_from_pixels(..., format) を呼ぶ
//     3. ★ 同じ image が両方の用途で参照される（稀）場合は **2枚登録する**。
//        1枚で兼ねることはできない（フォーマットは VkImageView 単位の性質なので）。
//        by_path_ のキーが {path, format} なのと同じ理屈。
//   ★ どの材質からも参照されていない image は読まなくてよい（スロットの節約）。
//
// (B) マテリアル変換で拾う項目を増やす
//     normalTexture.index            → MaterialTextures::normal
//     metallicRoughnessTexture.index → MaterialTextures::metallic_roughness
//     occlusionTexture.index         → MaterialTextures::occlusion
//     emissiveTexture.index          → MaterialTextures::emissive
//   ★ glTF の metallicRoughness テクスチャは **G=roughness, B=metallic**（R は未使用）。
//     逆に読むと「粗い金属」と「滑らかな誘電体」が入れ替わる。frag 側で間違えやすい。
//   ★ occlusion は **R チャンネル**。多くのモデルで MR テクスチャと同一画像に詰められている
//     （ORM テクスチャ: R=occlusion, G=roughness, B=metallic）。同じ image が
//     occlusionTexture と metallicRoughnessTexture の両方から参照される形になるが、
//     どちらも UNORM なので (A) の2枚登録は発生しない。
//
// (C) alphaMode の扱い
//     "BLEND" → LoadedMaterial::transparent = true（既存のまま）、alpha_cutoff = 0.0
//     "MASK"  → transparent = false、alpha_cutoff = material.alphaCutoff（既定 0.5）
//     "OPAQUE"→ transparent = false、alpha_cutoff = 0.0
//   ★ 「alpha_cutoff が 0 なら MASK 無効」という約束（②-2）を、ここで守る側が実装する。
//
// (D) 対応しないと明記するもの（今回も見送る）
//     normalTexture.scale / occlusionTexture.strength / doubleSided /
//     KHR_materials_* 拡張 / TEXCOORD_1 以降 / sampler のフィルタ・ラップ設定
//
// (E) NORMAL 欠損の扱いを直す【①-8 で可視化される問題】
//   現在は NORMAL が無いと全頂点 {0,1,0} で埋めている。ライティングが入ると
//   「全面が上を向いた、のっぺりした物体」として現れる。
//   → インデックスから**面法線を計算して頂点に累積 → 正規化**する
//     （③-4 の接線生成とほぼ同じループなので、一緒に書くと見通しがよい）。
//   ★ glTF 仕様では「NORMAL が無い場合はフラットシェーディングとみなす」とされている。
//     頂点を共有したまま平均化すると仕様と厳密には異なるが、このフェーズではそれでよい
//     （厳密にやるなら頂点を面ごとに複製することになる）。
```

### ③-6. シェーダでの法線マップ適用

```glsl
// ---- triangle.vert ----
layout(location = 2) in vec4 in_tangent;   // ★ location は graphics_pipeline.cpp と揃える

layout(location = 4) out vec3 frag_tangent;
layout(location = 5) out vec3 frag_bitangent;

//   mat3 nm = transpose(inverse(mat3(inst.model)));
//   vec3 N = normalize(nm * in_normal);
//   vec3 T = normalize(mat3(inst.model) * in_tangent.xyz);   // ★ 接線は逆転置ではなく model で変換する
//   ★ 接線は「面に沿った方向ベクトル」なので、法線とは変換の仕方が違う。
//     ここで逆転置を使うと非等方スケール時に接空間が歪む。
//   T = normalize(T - N * dot(N, T));                        // 直交化（補間で崩れるぶんの予防）
//   vec3 B = cross(N, T) * in_tangent.w;                     // ★ w の符号（③-4）

// ---- triangle.frag ----
//   vec3 n_tex = texture(textures[nonuniformEXT(m.normal_index)], frag_uv).xyz * 2.0 - 1.0;
//   mat3 TBN = mat3(normalize(frag_tangent), normalize(frag_bitangent), normalize(frag_normal));
//   vec3 N = normalize(TBN * n_tex);
//
//   vec2 mr = texture(textures[nonuniformEXT(m.metallic_roughness_index)], frag_uv).gb;
//   float roughness = m.roughness * mr.x;   // ★ G = roughness
//   float metallic  = m.metallic  * mr.y;   // ★ B = metallic
//   float ao = texture(textures[nonuniformEXT(m.occlusion_index)], frag_uv).r;
//   vec3 emissive = m.emissive.rgb * texture(textures[nonuniformEXT(m.emissive_index)], frag_uv).rgb;
//
// ★ 凹凸が逆に見えるときに疑う順:
//   1. in_tangent.w の符号（③-4）
//   2. TBN の列の順序（T, B, N の順。転置して掛けていないか）
//   3. 法線マップを sRGB で読んでいないか（D-6 / ③-2）
//   4. glTF の UV 原点は左上。V の反転を二重にしていないか
```

### ③-7. 検証
- Khronos サンプルの **`NormalTangentTest.gltf`** — 接空間の正誤が一目で分かる専用モデル。TANGENT 有り／無しの両方が入っている。
- `BoxTextured.gltf` が従来どおり表示されること（フォーマット引数化の回帰確認）。
- **`DamagedHelmet.glb`** — 65536 頂点超（uint32 インデックス）＋ 全テクスチャスロット入り。本フェーズの総合試験になる。
- 光源を動かしたとき、法線マップの凹凸の陰影が**正しい側に**動くこと。
- metallicRoughness テクスチャを持つモデルで、部位ごとに金属感が変わること（一様ならチャンネルの取り違えを疑う）。
- 組み込みジオメトリ（cube / plane）が接線追加後も正しく表示されること。

---

## 実装手順（この順で、各ステップ完了ごとに動作確認・コミット）

1. **⓪ 色空間** — スワップチェーンを SRGB へ → クリア色を linear へ → **元画像と画面の色が一致することを確認**
2. **① Blinn-Phong** — `Light` / `LightData` / `LightBuffer` → ディスクリプタ set=0 binding=2 → `CameraUBO` 拡張 → 収集フェーズにライト収集 → vert の法線行列 → frag の Blinn-Phong → main.cpp にライト配置 → **陰影が動くことを確認**
3. **② PBR** — frag の BRDF 差し替え → トーンマップ → マテリアルボールのシーン → **roughness / metallic の傾向を確認**（metallic=1 が黒いのは正常）
4. **③ 法線マップ・追加スロット** — `Texture` のフォーマット引数化 → flat normal テクスチャ → `MaterialData` 64 バイト化 + `MaterialTextures` → `Vertex::tangent` とパイプライン → glTF ローダー拡張（用途別フォーマット・追加スロット・NORMAL 欠損対処・TANGENT 生成）→ frag の TBN → **NormalTangentTest / DamagedHelmet で確認**

> **③ が難所**。中でも (a) テクスチャのフォーマット取り違え（落ちない・警告も出ない）、(b) 接空間の符号と掛け順、(c) MR テクスチャのチャンネル、の3つは**どれも「それらしく間違った絵」が出る**。2 まで終えた状態でコミットして区切ってから着手すること。
>
> ⓪ と ① の間で一度コミットしておくと、「暗さの原因がガンマかライトか」を後から二分探索できる。

---

## 検証観点（フェーズ全体）

- phase13/14 の性能特性が**維持**されていること（不透明がメッシュ種類数のドローに畳まれる、フラスタムカリングが効く、半透明の順序が保たれる）。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で描画とカメラ操作が継続すること。
- 終了時にバリデーションのリーク報告が無いこと。
- std140（カメラUBO）/ std430（インスタンス・マテリアル・ライト）のレイアウト関連エラーが出ないこと。
- ライト数 0 / 1 / 64（上限）/ 65（超過クランプ）で破綻しないこと。
- glTF モデルを大量（数十体）に配置しても破綻しないこと。

---

## Phase 16 で行うこと（このフェーズではやらない・確定事項）

1. **シャドウマップ** — 方向光1枚から。深度専用のレンダーパスとパイプライン、ライト視点の行列、比較サンプラ、PCF。点光はキューブマップなのでその次。
2. **IBL / 環境マップ** — D-8 で妥協した定数アンビエントの解消。金属が黒くなる問題はこれで解決する。irradiance map + prefiltered specular + BRDF LUT。
3. **参照カウントによるアセットの自動破棄** — phase14 ② で明示的 `unload()` のみにした部分。3つのレジストリの `Slot{ data, generation, alive }` が**完全に同形**になっているので、テンプレート基底 `AssetRegistry<T>` への括り出しと同時にやるのが自然。

## さらに先の将来課題（plan17以降）

- **法線行列の事前計算** — `InstanceData` に積む（Phase 15 ①-6 で頂点ごとの `inverse()` を許容した部分）。80 → 128 バイトになるので、帯域と計算量のトレードオフを測ってから決める。
- **球メッシュ / プリミティブの拡充**（Phase 15 ②-3 の TODO）。
- **スポットライト**と、ライトのカリング（Forward+ / Clustered）。現状は全ピクセル × 全ライトの総当たり。
- **専用トランスファーキュー**（キューファミリ跨ぎの所有権移譲）。
- **`GpuAllocator` → VMA 差し替え**（Phase 11 ③ で確保点を1箇所に閉じてある）。
- **OIT（順序独立透過）** — CPU ソートに依らない半透明。
- **キーコンフィグの設定ファイル入出力（JSON等）**（Phase 10 からの継続課題。nlohmann-json は tinygltf 経由で既に入っている）。
- **エンティティの生成・破棄を行う System**（`Registry&` を非constで受ける形への拡張）。
- **アニメーション / スキニング** — Phase 14 ③ で対応外にした部分。④ の階層が土台になる。
