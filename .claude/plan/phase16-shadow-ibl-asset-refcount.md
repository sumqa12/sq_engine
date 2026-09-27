# C++ ゲームエンジン（学習目的）— Phase 16: シャドウマップ + IBL + アセットの参照カウント

## Context

Phase 15 で**ライティングの式**が一通り揃った（[phase15-lighting-normal-mapping.md](phase15-lighting-normal-mapping.md) 参照）。色空間が linear で閉じ、Cook-Torrance PBR が回り、法線マップ・metallicRoughness・occlusion・emissive の4スロットが glTF から入る。`Duck.glb` の接線バグ（③-9）も潰し、モデル20体 × ライト65灯（上限クランプ）の負荷検証も通った。

一方で、phase15 が**意図的に残した穴**が3つある。どれも「妥協した」と plan に明記してあるものなので、そのまま本フェーズの材料になる。

| 穴 | どこで妥協したか | 今の症状 |
|---|---|---|
| **影が無い** | phase15 では扱わなかった | [triangle.frag](../../shaders/triangle.frag) の `result += (diff + specular) * radiance * NdotL /* * shadow */;` に**コメントで枠だけ**が残っている。物体が地面から浮いて見え、接地感が出ない |
| **環境光が定数** | phase15 **D-8** で明示的に妥協 | `const float kAmbient = 0.03;`。**metallic=1 のマテリアルがほぼ真っ黒**になる（金属は拡散反射を持たず、鏡面は映り込みでしか得られないため） |
| **アセットが解放されない** | phase14 ② で「明示的 `unload()` のみ」にした | 誰も参照しなくなったメッシュ・テクスチャ・マテリアルが GPU に残り続ける。3つのレジストリの `Slot{ data, generation, alive }` は**完全に同形**なのに、実装も3箇所に重複している |

さらに、本フェーズは**このエンジンが一度も触っていない Vulkan の要素**を6つまとめて通すことになる。ここが phase16 の重さの本体で、絵の話はその上に乗るだけである。

| 初めて触るもの | どこで要るか |
|---|---|
| **オフスクリーンのレンダーパス**（スワップチェーンに紐づかない描画先） | ① シャドウマップ |
| **深度専用パイプライン**（カラーアタッチメント 0 本・フラグメントシェーダ無し） | ① |
| **比較サンプラ**（`compareEnable = VK_TRUE` / `sampler2DShadow`） | ① |
| **動的ステート depth bias**（`vkCmdSetDepthBias`） | ① |
| **キューブマップ**（`arrayLayers = 6` / `VIEW_TYPE_CUBE`） | ② IBL |
| **コンピュートパイプライン**（このエンジンで初。ストレージイメージ・`vkCmdDispatch`） | ② |

例によって Claude は宣言・骨格・TODO コメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-0. 着手順は ⓪ → ① → ② → ③

| 順 | 項目 | 理由 |
|---|---|---|
| 0 | **⓪ オフスクリーン描画の土台** | `RenderPass` / `GraphicsPipeline` / `Sampler` はどれも**今の用途に決め打ち**で書かれている。①②で使う前に、先に「設定を外から渡せる形」へ広げておく。**絵は1ピクセルも変わらない**ので、ここで壊れたら原因は確実にリファクタ側 |
| 1 | **① シャドウマップ** | ⓪で作った土台の最初の利用者。方向光1枚・正射影・PCF まで |
| 2 | **② IBL** | ①とは独立（描画先が増えるのは同じだが、①はラスタライズ、②はコンピュート）。①が通っていれば set=2 の配管は済んでいる |
| 3 | **③ AssetRegistry\<T\> と参照カウント** | ①②と**完全に独立**。絵が変わらない純粋なリファクタなので最後に置く |

**⓪ を切り出す理由**: `RenderPass` はカラー+深度の2アタッチメント決め打ち、`GraphicsPipeline` は「カラーブレンド1本・フラグメントシェーダ必須・depth bias 無効」決め打ち、`Sampler` は引数を1つも取らない。この3つを広げる作業と「影の式が合っているか」を同じコミットでやると、絵が出なかったときに疑う場所が倍になる。**⓪ の完了条件は「今までと完全に同じ絵が出ること」**。

**③ を最後に置く理由**: 独立なので順序は入れ替えてよいが、絵が変わる作業を先に済ませた方が、途中で止めたときに残るものが大きい。

### D-1. 新しいディスクリプタセット set=2「ライティング環境」を作る

phase14 D-2 / phase15 D-3 で決めた set の役割分担を、そのまま1段拡張する。

| set | 性格 | 中身 | 個数 |
|---|---|---|---|
| **set=0** | フレームごとに変わる | binding=0: カメラUBO<br>binding=1: インスタンス SSBO<br>binding=2: ライト SSBO | `kFramesInFlight` 個 |
| **set=1** | 起動後は不変（アセット） | binding=0: bindless テクスチャ配列<br>binding=1: マテリアル SSBO | 全体で1個 |
| **set=2** | **ライティング環境（← 今回追加）** | binding=0: **シャドウマップ**（`sampler2DShadow`）<br>binding=1: **irradiance キューブ**<br>binding=2: **prefiltered specular キューブ**<br>binding=3: **BRDF LUT** | `kFramesInFlight` 個 |

**set=1（bindless 配列）に入れられない理由**が3つあり、どれも単独で決定的:

1. **キューブマップは `sampler2D[]` に入らない**。`samplerCube` は別の型で、同じバインディング配列に混ぜられない。
2. **シャドウマップは比較サンプラが要る**。set=1 の配列は全要素が `Renderer::sampler_`（`compareEnable = VK_FALSE`）を共有している。`sampler2DShadow` は `compareEnable = VK_TRUE` のサンプラでしか使えない。
3. **シャドウマップはフレームごとに実体が変わる**（D-2）。set=1 は「起動後は不変」という前提で1個しか作っていない。

**set=2 を `kFramesInFlight` 複製する理由**: 中身のうちシャドウマップだけがフレームごとに変わる。IBL の3つはフレーム間で同じ実体を指すので、ディスクリプタ枠を `kFramesInFlight` 倍ぶん無駄に食うが、**「set=0 と同じ規則で扱える」ことの方が価値が大きい**（4枠 × 2フレーム = 8枠の消費に過ぎない）。IBL だけを set=3 に分ける案は、セットが増えるわりに得るものが無い。

- ★ **プールの更新を忘れないこと**（⓪-4）。`maxSets` は `kFramesInFlight + 1` → `kFramesInFlight * 2 + 1`、`COMBINED_IMAGE_SAMPLER` の枠は `kMaxTextures` → `kMaxTextures + kFramesInFlight * 4`。忘れると `vkAllocateDescriptorSets` が `VK_ERROR_OUT_OF_POOL_MEMORY` を返す（現在の実装は戻り値を見ていないので、**そのまま VK_NULL_HANDLE のセットをバインドして落ちる**）。

### D-2. シャドウマップは `kFramesInFlight` 個持つ

1枚で済ませたくなるが、**フレーム N のシャドウパスが書き込む相手を、まだ GPU 上で実行中のフレーム N-1 の本パスが読んでいる**可能性がある。`kFramesInFlight = 2` なので2枚。

- カメラUBO・インスタンス SSBO・ライト SSBO と**まったく同じ理由・同じ形**（phase13 D-5）。新しい話は何も無い。
- ★ 「同じコマンドバッファの中で書いて読むのだから安全では？」は**フレーム内では正しいがフレーム間では誤り**。コマンドバッファの提出順は守られても、実行の重なりは止まらない。止めるのはフェンスであり、フェンスは `current_frame_` 単位でしか待っていない。

### D-3. 光源空間行列は `CameraUBO` に入れる（プッシュ定数は使わない）

シャドウパスの頂点シェーダと、本パスのフラグメントシェーダの**両方**が同じ `light_view_projection` を要る。置き場の候補は2つ:

- (a) **`CameraUBO` に `mat4` を1本足す**（96 → 160 バイト）
- (b) シャドウパスはプッシュ定数で渡し、本パスは UBO から読む

**(a) を採る**。理由:

1. **同じ値を2経路で運ばない**。phase15 ③-9 バグA の教訓（中立値の表を2箇所に置かない）がそのまま当てはまる。プッシュ定数側だけ更新して UBO を忘れれば、「影の位置だけがずれる」という切り分けの難しい壊れ方になる。
2. **パイプラインレイアウトが全パスで同一になる**。これが効く: プッシュ定数レンジが違うパイプライン同士は**「レイアウト互換」ではなくなり、バインド済みのディスクリプタセットが無効化される**。(b) を採ると、シャドウパスで set=0 をバインド → 本パスのパイプラインへ切り替えた瞬間に set=0 が外れる、という極めて追いにくいバグの下地ができる。
3. `mat4` 1本 = 64 バイトは、プッシュ定数の保証下限（128 バイト）を半分埋める。将来カスケード（4分割）にすると即座に溢れる。

- ★ プッシュ定数は phase16 では**使わない**。素直な初使用は「点光源のキューブシャドウで、6面それぞれの view-projection を面ごとに渡す」場面（phase17）。
- ★ `CameraUBO` が 160 バイトになるので `static_assert(sizeof(CameraUBO) == 160)` を更新すること。忘れると**アサートで止まる**＝気付ける壊れ方なので、これは良い設計になっている。

### D-4. 影を落とすのは方向光1つだけ。どのライトかは `CameraUBO::light_count.y` で渡す

- `scene::Light` に `bool cast_shadows = false;` を足す。収集フェーズで**最初に見つかった `cast_shadows == true` の Directional ライト**をシャドウキャスタに選ぶ。
- そのライトが `lights_` 配列の何番目かを **`CameraUBO::light_count.y`** に入れる。見つからなければ `~0u`。
  - `light_count` は `glm::uvec4` で **`.x` しか使っていない**（phase15 ①-4）。`.y` が空いているのはこのためでもある。`LightData` に 4 本目の `vec4` を足すより、こちらの方が構造体のレイアウトを触らずに済む。
- フラグメントシェーダは、ライトのループの中で `i == camera.light_count.y` のときだけシャドウ係数を掛ける。

```glsl
float shadow = 1.0;                      // 1.0 = 照らされている
if (i == camera.light_count.y) { shadow = sample_shadow(frag_world_pos, N, L); }
result += (diff + specular) * radiance * NdotL * shadow;
```

- ★ **点光源に影を付けない理由**: 点光源はキューブマップ（6面）になり、パスが6倍・行列が6本・`VK_IMAGE_VIEW_TYPE_CUBE` の深度イメージ・`samplerCubeShadow` と、方向光とは別物の作業量になる。phase17 へ回す。
- ★ **`cast_shadows` を足さず「最初の Directional」を自動で選ぶ**案もあるが、採らない。検証シーンで「影を出すライト」と「色を確認するライト」を分けたくなるため。

### D-5. シャドウキャスタは**ライトの箱で別途カリング**し、同じインスタンス SSBO の第3区間に詰める

現在 `instances_` は「不透明 → 半透明」の2区間で、`record_draw_items` の `first_instance` がその境目になっている。ここに**第3区間「シャドウキャスタ」**を足す。

```
instances_ = [ ---- opaque ---- | ---- transparent ---- | ---- shadow casters ---- ]
                 first = 0          first = opaque.size()    first = opaque + transparent
```

- **採らない案: カメラのカリング結果（`opaque_items_`）をそのままシャドウパスにも使う**。1行で済むが、**画面外の物体の影が画面内に落ちない**。カメラを回すと影が消えたり湧いたりするので、症状としては派手で気付きやすいが、正しくない。
- 正しくは**ライト側の視錐台**（方向光なら正射影の箱）でカリングする。`scene::Frustum::from_view_projection(light_view_projection)` が**そのまま使える**（[frustum.hpp](../../engine/include/sq/scene/frustum.hpp)。正射影でも平面6枚の抽出式は同じ）。
- ★ 半透明はシャドウキャスタに**含めない**。深度しか書かないので、半透明のガラスが不透明の真っ黒な影を落とすことになる。含めるなら alpha 対応（D-6）が先。
- ★ 区間を足すと `kMaxInstances` の超過クランプ処理（[renderer.cpp](../../engine/src/graphics/renderer.cpp) の `instances_.size() > kMaxInstances` の分岐）も3区間ぶんに直す必要がある。**忘れると 4096 体を超えた瞬間に、転送していない範囲を描く**。

### D-6. シャドウパスはフラグメントシェーダを持たない

- 深度しか要らないので `stageCount = 1`（頂点シェーダのみ）、サブパスの `colorAttachmentCount = 0`。
- ★ **代償**: `alpha_cutoff`（MASK モード）のマテリアルが**穴の開いていない影**を落とす。葉のテクスチャを貼った板が四角い影になる。
- ★ 直すなら「シャドウ用のフラグメントシェーダで `discard` する」だけだが、そのためには set=1（マテリアル・テクスチャ）をシャドウパスでもバインドし、`material_index` を頂点シェーダから渡す必要がある。**このフェーズではやらない**。現在のアセットに MASK マテリアルが無いため症状が出ない。

### D-7. IBL の前計算は**コンピュートシェーダ**で行う

irradiance / prefiltered specular / BRDF LUT の3枚（と equirectangular → キューブの変換）をどう作るか。

- **採らない案: キューブの6面へレンダリングする**（定番手順）。必要になるもの: オフスクリーンのカラーレンダーパス、面ごとの `VkImageView`（または層付きフレームバッファ）、面ごとの `VkFramebuffer`、立方体メッシュ、面ごとの view 行列、ビューポート設定、mip レベルごとのフレームバッファ再生成。
- **採る案: コンピュートシェーダ**。必要になるもの: `ComputePipeline` クラス1つ、ストレージイメージのディスクリプタ、`vkCmdDispatch`。
  - **ラスタライズの都合（三角形・ビューポート・フレームバッファ）が全部消える**。やりたいことは「出力テクセルごとに積分する」なので、計算モデルとしてもコンピュートの方が素直。
  - `gl_GlobalInvocationID.xy` が出力テクセル、`.z` が**キューブの面番号**（`vkCmdDispatch(w/8, h/8, 6)`）。6面が1回のディスパッチで済む。
  - コンピュートパイプラインはこのエンジンに無いので**新規に1クラス増える**が、レンダーパス・フレームバッファ・メッシュが不要になるぶん、差し引きで書く量は減る。
- ★ **キューを増やす必要は無い**。グラフィックスキューファミリは実質すべての GPU で `VK_QUEUE_COMPUTE_BIT` を持つ。ただし**確認はすること**（`PhysicalDeviceSelector::find_queue_families` で `graphics_family` を選ぶときに COMPUTE ビットも要求する）。専用のコンピュートキューは将来課題。
- ★ 前計算は**起動時に1回だけ**。`SingleTimeCommands`（既存）に載せて、完了を待ってから次へ進む。毎フレームの経路には一切入らない。

### D-8. 環境マップは HDR の equirectangular を読んでキューブへ変換する

- 入力は `.hdr`（Radiance RGBE）。`stb_image` の **`stbi_loadf`** が float で返す（vcpkg の `stb` は導入済み）。
- 内部フォーマットは **`VK_FORMAT_R16G16B16A16_SFLOAT`**。理由が2つある:
  1. **HDR だから**（1.0 を超える値を保持する必要がある。太陽の周辺は 10 や 100 になる）
  2. **ストレージイメージに sRGB フォーマットは使えない**。コンピュートから `imageStore` する以上、`*_SRGB` は選択肢から消える。
- ★ キューブの `VkImage` には**2種類のビューを作る**:
  - サンプル用: `VK_IMAGE_VIEW_TYPE_CUBE`、`layerCount = 6`
  - 書き込み用: `VK_IMAGE_VIEW_TYPE_2D_ARRAY`、`layerCount = 6`（`imageStore` は配列として書く）
  - prefiltered specular は**mip レベルごとに書き込み用ビューが要る**（`imageStore` は mip を指定できない。どの mip に書くかは**ビューが決める**）。
- ★ `VkImageCreateInfo::flags` に **`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`** を立てること。忘れると `VIEW_TYPE_CUBE` のビュー作成がバリデーションエラーになる。
- ★ `usage` は `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`。ストレージとして書くときのレイアウトは **`VK_IMAGE_LAYOUT_GENERAL`**、書き終えたら `SHADER_READ_ONLY_OPTIMAL` へ遷移させる。

### D-9. 参照カウントは RAII ハンドル `AssetRef<T>` で持つ。ECS コンポーネントは生ハンドルのまま

「誰が参照を数えるか」が ③ の本質的な設計判断で、選択肢は3つ:

- (a) 生ハンドル + 明示的な `retain()` / `release()` … 必ず呼び忘れる
- (b) **RAII ハンドル `AssetRef<T>`**（コピーで +1、破棄で -1） … 採用
- (c) 毎フレーム ECS を走査して参照元を数える … 走査コストが参照の数に比例し、しかも「ECS の外からの参照」を数えられない

**(b) を採る。ただし ECS コンポーネント（`scene::MeshHandle` / `scene::Material`）は生ハンドルのまま残す。**

- 生ハンドルを残す理由: GPU へ渡すのは `id.index` という生の整数で、レンダラは毎フレームこれを読む。またコンポーネントは `ComponentStorage<T>`（= `std::vector<T>`）に載っていてアーキタイプ間を移動するため、**ムーブのたびに参照カウントの増減が正しいか**を気にする範囲が広がる。
  - ★ 技術的には可能である。[component_storage.hpp](../../engine/include/sq/ecs/component_storage.hpp) は `std::vector<T>` のムーブ代入と `pop_back` で実装されており、非自明なデストラクタを持つ型を正しく扱う。`Registry::destroy` も存在する。**「できない」のではなく「所有の意味が違う」から分ける**。
- **「エンティティがアセットを参照している」ことと「エンティティがアセットを所有している」ことは別**、というのが分け方の根拠。所有するのは「そのアセット群をロードした主体」であり、それは `assets::LoadedModel` かアプリ側である。
- ★ **参照カウントを間違えても静かには壊れない**。世代番号（phase14 D-5）があるので、早すぎる解放は `contains()` が `false` を返し、既定メッシュ／既定マテリアルへフォールバックする。市松模様のテクスチャが出たら「参照カウントが足りていない」と読める。**この安全網があることが、③ を後回しにできた理由でもある。**

`AssetRef` を持つのは3者だけ:

| 持ち主 | 何を所有するか |
|---|---|
| `assets::LoadedModel` | そのファイルが登録したメッシュ・マテリアル一式 |
| `MaterialRegistry::Slot` | そのマテリアルが使うテクスチャ5枚（**マテリアル → テクスチャの連鎖解放**） |
| アプリ側（`main.cpp`） | 手で組んだメッシュ・テクスチャ・マテリアル |

- ★ **循環参照は起きない**。依存は `Material → Texture` の一方向だけで、`Texture` は何も参照しない。将来もこの向きを崩さないこと。

### D-10. `AssetRegistry<T>` は素直な仮想関数で括る（CRTP を採らない）

- 3つのレジストリの差分は**「スロットを解放するときに何をするか」だけ**（③-1 の表）。純粋仮想 `on_destroy(index, Entry&&)` 1本で吸収できる。
- **採らない案: CRTP**（`template <class Derived, class Handle, class Entry> class AssetRegistry`）。vtable が消えるが、
  - 解放は「アセットをアンロードしたとき」しか走らない。**毎フレームの経路に一度も現れない**ので、仮想呼び出しのコストを気にする理由が無い。
  - CRTP はエラーメッセージが劇的に読みにくくなる。テンプレート基底を書くのが初めてなら、まず仮想関数で通すこと。
- ★ テンプレートなのでヘッダオンリーになる（`engine/include/sq/graphics/asset_registry.hpp`）。`.cpp` は作らない。

---

## ⓪ オフスクリーン描画の土台

**このステップの完了条件は「今までと完全に同じ絵が出ること」。** 1ピクセルでも変わったらリファクタが壊れている。

### ⓪-1. `RenderPass` を深度専用にも作れるようにする（[render_pass.hpp](../../engine/include/sq/graphics/render_pass.hpp) / [.cpp](../../engine/src/graphics/render_pass.cpp)）

```cpp
// render_pass.hpp に追加する設定構造体（PipelineConfig と同じ発想）。
//
// 現在の RenderPass は「カラー1枚 + 深度1枚、カラーの finalLayout は PRESENT_SRC」で
// 決め打ちになっている。シャドウパスは「深度1枚だけ、finalLayout は
// DEPTH_STENCIL_READ_ONLY_OPTIMAL（この後フラグメントシェーダが読むため）」が要る。
struct RenderPassConfig {
    bool has_color = true;   // false なら深度専用（colorAttachmentCount = 0）

    // 深度アタッチメントの最終レイアウト。
    //   本パス     : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL（今までどおり）
    //   シャドウパス: VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    VkImageLayout depth_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // 深度の storeOp。
    //   本パス     : DONT_CARE（描き終わったら捨てる。今までどおり）
    //   シャドウパス: STORE   ★ 中身が成果物なので、ここを DONT_CARE にすると
    //                          「影が出ない／ノイズになる」。一番忘れやすい1行
    VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
};

// 既存のコンストラクタに config を足す（既定値は今の挙動と同じにして、呼び出し側を壊さない）。
RenderPass(VkDevice device, VkFormat color_format, VkFormat depth_format,
           const RenderPassConfig& config = {});
```

```
// .cpp 側で直すところ:
//   1. has_color == false なら attachments に color_attachment を入れない
//      → 深度が attachment 0 になる（depth_attachment_reference.attachment = 0）
//   2. subpass.colorAttachmentCount = has_color ? 1 : 0;  pColorAttachments も nullptr に
//   3. depth_attachment の storeOp / finalLayout を config から取る
//   4. サブパス依存を2本にする（深度専用のとき）:
//      [0] EXTERNAL -> 0 : srcStage = FRAGMENT_SHADER,               dstStage = EARLY_FRAGMENT_TESTS
//                          srcAccess = SHADER_READ,                  dstAccess = DEPTH_STENCIL_ATTACHMENT_WRITE
//          （前フレームの読み取りが終わってから書き始める）
//      [1] 0 -> EXTERNAL : srcStage = LATE_FRAGMENT_TESTS,           dstStage = FRAGMENT_SHADER
//                          srcAccess = DEPTH_STENCIL_ATTACHMENT_WRITE, dstAccess = SHADER_READ
//          （書き終わってから本パスのフラグメントシェーダが読む）
//      ★ [1] が無いと「影がちらつく／1フレーム遅れる／環境によっては正しく見える」
//        という、再現しにくい壊れ方をする。**バリデーションは出ない**。
```

### ⓪-2. `GraphicsPipeline` の設定を広げる（[graphics_pipeline.hpp](../../engine/include/sq/graphics/graphics_pipeline.hpp) / [.cpp](../../engine/src/graphics/graphics_pipeline.cpp)）

```cpp
// PipelineConfig に足すメンバ（既定値はすべて「現在の挙動」にすること）。
struct PipelineConfig {
    bool depth_write_enable = true;
    bool blend_enable = false;

    // ★ 以下は phase16 ⓪-2 で追加 --------------------------------

    // カラーアタッチメントの有無。false で colorBlendState の attachmentCount = 0。
    // シャドウパス（深度専用）で false にする。
    // ★ レンダーパス側の colorAttachmentCount と**必ず一致**させること。
    //   食い違うとパイプライン作成時にバリデーションエラー（これは気付ける）。
    bool has_color_attachment = true;

    // depth bias（シャドウアクネ対策。①-8）。true なら
    // VK_DYNAMIC_STATE_DEPTH_BIAS も動的ステートに加え、値は vkCmdSetDepthBias で渡す。
    // ★ 動的にする理由: 適正値はシーンと解像度で変わる。パイプラインに焼くと
    //   1回試すたびに再ビルドになる。
    bool depth_bias_enable = false;

    // 背面/前面カリング。シャドウパスは VK_CULL_MODE_FRONT_BIT にするとアクネが減る
    // （①-8。代わりに peter-panning が出る）。スカイボックス（②-9）でも使う。
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;

    // 深度比較。スカイボックスは LESS_OR_EQUAL（深度 1.0 で描くため。②-9）。
    VkCompareOp depth_compare_op = VK_COMPARE_OP_LESS;

    // 頂点入力の有無。false なら vertexBindingDescriptionCount = 0 /
    // vertexAttributeDescriptionCount = 0 にする。
    // ②-9 のフルスクリーン三角形（gl_VertexIndex から頂点を作る）で使う。
    bool has_vertex_input = true;
};

// コンストラクタの frag_spv_path を「空文字列ならフラグメントステージを作らない」仕様にする。
//   ★ 既定引数にはしないこと（phase15 D-6 の VkFormat と同じ理由。
//     「書き忘れ」が黙って通ると、カラー出力の無いパイプラインが意図せず作られる）。
//   ★ stageCount は「frag が空なら 1、そうでなければ 2」。
```

### ⓪-3. `Sampler` を設定可能にする（[sampler.hpp](../../engine/include/sq/graphics/sampler.hpp) / [.cpp](../../engine/src/graphics/sampler.cpp)）

```cpp
// 現在の Sampler は引数を1つも取らず、全テクスチャ共有の1本だけを作る前提。
// phase16 では**3本目まで増える**:
//   1. 既存の共有サンプラ（REPEAT / 異方性あり / 比較なし）
//   2. シャドウ用（CLAMP_TO_BORDER / 白ボーダー / 比較あり）      ← ①
//   3. キューブ・LUT 用（CLAMP_TO_EDGE / 異方性なし / 比較なし）  ← ②
struct SamplerConfig {
    VkFilter filter = VK_FILTER_LINEAR;
    VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkBorderColor border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    bool anisotropy = true;

    // 比較サンプラ（シャドウ用）。true なら compareEnable = VK_TRUE。
    // ★ compare_op は VK_COMPARE_OP_LESS_OR_EQUAL。
    //   「サンプルした深度 <= 比較値 なら 1.0（= 手前にいる = 照らされている）」。
    //   GREATER にすると**影と光が反転**し、しかも「それらしい絵」が出るので気付きにくい。
    bool compare_enable = false;
    VkCompareOp compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;

    float max_lod = VK_LOD_CLAMP_NONE;
};

Sampler(VkPhysicalDevice physical_device, VkDevice device, const SamplerConfig& config = {});
```

- ★ **シャドウサンプラのアドレスモードは必ず `CLAMP_TO_BORDER` + 白ボーダー**にすること。
  - 白 = 深度 1.0 = far = 「そこには何も無い」= 影なし。シャドウマップの外へ出たフラグメントが自動的に「照らされている」扱いになる。
  - `REPEAT` のままだと**影がシーン全体にタイル状に繰り返す**。派手なので気付けるが、`CLAMP_TO_EDGE` にすると「箱の外側に縁の影が帯状に伸びる」という中途半端な壊れ方になり、こちらは原因を見失いやすい。

### ⓪-4. set=2 のレイアウトとプールの増枠（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）

```
// create_descriptor_set_layout() に3つ目のレイアウトを足す（D-1）。
//   environment_set_layout_:
//     binding=0  COMBINED_IMAGE_SAMPLER × 1   FRAGMENT   シャドウマップ
//     binding=1  COMBINED_IMAGE_SAMPLER × 1   FRAGMENT   irradiance cube
//     binding=2  COMBINED_IMAGE_SAMPLER × 1   FRAGMENT   prefiltered cube
//     binding=3  COMBINED_IMAGE_SAMPLER × 1   FRAGMENT   BRDF LUT
//   ★ UPDATE_AFTER_BIND も PARTIALLY_BOUND も不要（バインド前に全部書き終える）。
//
// create_descriptor_pool() の更新:
//   maxSets:                 kFramesInFlight + 1  →  kFramesInFlight * 2 + 1
//   COMBINED_IMAGE_SAMPLER:  kMaxTextures         →  kMaxTextures + kFramesInFlight * 4
//   ★ 現在の実装は vkCreateDescriptorPool / vkAllocateDescriptorSets の戻り値を
//     **見ていない**。ここを直すついでに VK_SUCCESS の確認と throw を入れること。
//     入れないと、プール枯渇が「VK_NULL_HANDLE のセットをバインドして落ちる」
//     という原因の見えない形で表面化する。
//
// create_descriptor_sets():
//   environment_sets_（kFramesInFlight 個）を確保する。
//   ★ 中身を書くのは ① と ② がそれぞれの実体を作った後。この時点では確保だけ。
//     「確保しただけで一度も書いていないセット」をバインドすると
//     PARTIALLY_BOUND が無いので不正になる。⓪ の段階では**バインドもしないこと**。
//
// パイプライン生成箇所:
//   set_layouts = { camera_set_layout_, material_set_layout_, environment_set_layout_ }
//   ★ 3本すべてのパイプライン（不透明・半透明・シャドウ）で同じ配列を使う（D-3 の理由2）。
//
// デストラクタ:
//   vkDestroyDescriptorSetLayout(environment_set_layout_) を足す。
```

### ⓪-5. 検証

- **絵が phase15 と完全に同じであること**（ここが完了条件）。
- バリデーション層に新しい警告が1つも出ないこと。
- リサイズ・F11・Alt+Tab 復帰が従来どおり動くこと。
- ★ set=2 はまだ**バインドしない**。レイアウトとプール枠だけがある状態。

---

## ① シャドウマップ（方向光1枚 + PCF）

### ①-1. `draw_frame` の分解（[renderer.cpp](../../engine/src/graphics/renderer.cpp)）

**ここが ① で最初にやること。** 現在の `draw_frame` は、`command_buffers_->record(...)` のラムダの中で
`vkCmdBeginRenderPass` → カメラ解決 → ライト収集 → 描画アイテム収集 → ソート → 転送 → 記録 → `vkCmdEndRenderPass`
を**全部やっている**。シャドウパスは本パスの**前**に置く必要があるので、この順序を組み替える。

```
// 組み替え後の draw_frame（ラムダの中身）:
//
//   1. カメラの解決（aspect / view_projection / cam_pos）
//   2. ライトの収集 → lights_ / シャドウキャスタライトの選択（①-3）
//   3. 光源空間行列の計算（①-3）
//   4. カメラUBO の更新（light_view_projection と light_count.y を含む）
//   5. 描画アイテムの収集（不透明 / 半透明）← カメラ視錐台でカリング
//   6. シャドウキャスタの収集（①-5）      ← ライト視錐台でカリング
//   7. ソート
//   8. instances_ の組み立て（3区間）と転送
//   -------- ここまでレンダーパスの外 --------
//   9.  シャドウパス   : BeginRenderPass(shadow) → 記録 → EndRenderPass
//   10. 本パス         : BeginRenderPass(main)   → 記録 → EndRenderPass
//
// ★ ディスクリプタセットのバインド位置に注意:
//     set=0 は 9 と 10 の**両方**で要る（シャドウ頂点シェーダもインスタンス SSBO を読む）。
//     set=1 と set=2 は 10 でだけバインドする。
//   ★ set=2 を 9 より前にバインドしてはいけない。set=2 binding=0 は
//     いままさに深度アタッチメントとして書き込んでいる当のイメージなので、
//     バリデーションが「descriptor image layout mismatch」を出す。
//     **バインドしなければ問題にならない**（レイアウトに含まれているだけなら無害）。
//
// ★ 抽出のしどころ: 1〜8 は「フレームのデータを作る」、9〜10 は「コマンドを記録する」。
//   ラムダが 200 行を超えるので、collect_frame_data() / record_shadow_pass() /
//   record_main_pass() のような private メソッドへ分けること。
//   分けないと ② のスカイボックス追加でさらに膨らむ。
```

### ①-2. `ShadowMap` クラス（新規 `shadow_map.hpp` / `.cpp`）

```cpp
// シャドウマップ1枚ぶんの GPU リソース（phase16 ①-2）。
//
// DepthImage（[depth_image.hpp](../../engine/include/sq/graphics/depth_image.hpp)）と
// よく似ているが、3つ違う:
//   1. usage に VK_IMAGE_USAGE_SAMPLED_BIT が要る（フラグメントシェーダが読むため）
//   2. 解像度がスワップチェーンと無関係（固定。リサイズで作り直さない）
//   3. VkFramebuffer も自分で持つ（描画先が1枚しか無いので、外に出す意味が無い）
//
// ★ DepthImage を継承したり流用したりしないこと。「深度イメージ」という共通点は
//   あるが、寿命の管理（片方はリサイズで作り直す・片方は作り直さない）が逆なので、
//   共通化すると recreate_swapchain が汚れる。
class ShadowMap {
public:
    // size: 一辺のテクセル数（正方形）。2048 から始める（①-8 で調整する）。
    // depth_format: Renderer が持っている depth_format_ をそのまま渡す。
    //   ★ D24_UNORM_S8_UINT が選ばれている環境では、ビューの aspectMask を
    //     VK_IMAGE_ASPECT_DEPTH_BIT だけにすること（STENCIL を混ぜると
    //     サンプル可能なビューにならない）。
    ShadowMap(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
              VkRenderPass shadow_render_pass, std::uint32_t size, VkFormat depth_format);
    ~ShadowMap();  // framebuffer -> view -> image -> memory の順で破棄

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    [[nodiscard]] VkImageView view() const;        // set=2 binding=0 に書く
    [[nodiscard]] VkFramebuffer framebuffer() const;
    [[nodiscard]] VkExtent2D extent() const;       // vkCmdSetViewport / renderArea に使う

private:
    GpuAllocator* allocator_ = nullptr;   // 所有しない（Device が所有）
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
};
```

Renderer 側:

```
// renderer.hpp に足すメンバ:
//   static constexpr std::uint32_t kShadowMapSize = 2048;
//   std::unique_ptr<RenderPass> shadow_render_pass_;
//   std::vector<std::unique_ptr<ShadowMap>> shadow_maps_;   // kFramesInFlight 個（D-2）
//   std::unique_ptr<GraphicsPipeline> pipeline_shadow_;
//   std::unique_ptr<Sampler> shadow_sampler_;               // 比較サンプラ（⓪-3）
//   VkDescriptorSetLayout environment_set_layout_ = VK_NULL_HANDLE;
//   std::vector<VkDescriptorSet> environment_sets_;         // kFramesInFlight 個
//
// 構築順（既存の番号付きコメントに差し込む）:
//   7. render_pass_ の直後に shadow_render_pass_ を作る
//   10. create_sampler() で shadow_sampler_ も作る
//   14. パイプライン生成で pipeline_shadow_ も作る
//        PipelineConfig{ .depth_write_enable = true, .blend_enable = false,
//                        .has_color_attachment = false, .depth_bias_enable = true,
//                        .cull_mode = VK_CULL_MODE_FRONT_BIT }
//        頂点シェーダ "shaders/shadow.vert.spv"、フラグメントは "" （D-6）
//   15. create_framebuffers() の前後で shadow_maps_ を作る
//
// ★ shadow_maps_ は recreate_swapchain() で作り直さないこと（解像度がウィンドウと無関係）。
//   作り直すと、ディスクリプタに書いた view が死んで「リサイズすると影が壊れる」。
```

### ①-3. `Light::cast_shadows` と光源空間行列

```cpp
// light.hpp の scene::Light に足す:

    // このライトが影を落とすか（phase16 ①-3 / D-4）。
    //
    // ★ 実際に影を落とせるのは **Directional かつ cast_shadows == true のうち最初の1つ**だけ。
    //   2つ目以降は無視される（警告を出すこと。黙って無視すると
    //   「なぜこのライトだけ影が出ないのか」で時間を溶かす）。
    // ★ Point で立てても無視される（キューブシャドウは phase17）。
    bool cast_shadows = false;
```

```cpp
// 光源空間行列の計算（renderer.cpp の収集フェーズ、または scene 側のフリー関数）。
//
// 方向光は「無限遠から平行に来る光」なので、**正射影**を使う。
// 透視投影にすると光が1点から広がることになり、平行光の定義と矛盾する。
//
//   glm::vec3 light_dir = 光の進む向き（= -world.matrix[2] の逆、正規化済み）
//   glm::vec3 center    = 影を出したい範囲の中心
//   glm::vec3 eye       = center - light_dir * kShadowDistance
//   glm::mat4 view      = glm::lookAt(eye, center, up)
//   glm::mat4 proj      = glm::ortho(-E, E, -E, E, kShadowNear, kShadowFar)
//   light_view_projection = proj * view
//
// ★ up の縮退に注意。light_dir がほぼ ±Y だと lookAt の内部の cross がゼロになり
//   行列が NaN になる。main.cpp の create_directional_light が同じ問題を
//   分岐で避けている（|axis.y| > 0.99 なら参照上方向を Z にする）。**同じ分岐が要る**。
//   NaN 行列になると影ではなく**画面全体が真っ黒**になるので症状は派手。
//
// ★ GLM_FORCE_DEPTH_ZERO_TO_ONE がプロジェクト全体で定義済みなので、
//   glm::ortho の z 範囲も [0, 1] になる。Vulkan と一致しているので追加の補正は不要。
//
// ★ center をどこにするか（このフェーズの割り切り）:
//     v1 = 原点固定 + 十分大きな E（kShadowOrthoExtent = 40 程度）
//   これで「シーン全体が1枚に収まるが、遠景では 2048 テクセルを分け合うので粗い」状態になる。
//   カメラ追従にすると「カメラが動くと影の縁がちらつく（シマリング）」が出る。
//   直すにはテクセル単位のスナップが要り、それはカスケード（phase17）とセットでやる方がよい。
//   **v1 では原点固定にして、ちらつきの問題そのものを先送りする。**
```

```cpp
// renderer.hpp に足す定数:
//   static constexpr float kShadowOrthoExtent = 40.0f;  // 正射影の半幅
//   static constexpr float kShadowNear        = 0.1f;
//   static constexpr float kShadowFar         = 200.0f;
//   static constexpr float kShadowDistance    = 100.0f; // center からライトを引く距離
//   ★ kShadowDistance < kShadowFar / 2 くらいにしておくこと。
//     近すぎるとシーンの手前側が near でクリップされ、「手前の物体の影だけ消える」。
```

### ①-4. `CameraUBO` の拡張（[camera.hpp](../../engine/include/sq/scene/camera.hpp)）

```cpp
struct CameraUBO {
    glm::mat4  view_projection;        // offset   0, 64
    glm::mat4  light_view_projection;  // offset  64, 64   ← phase16 ①-4 で追加（D-3）
    glm::vec4  camera_position;        // offset 128, 16   xyz = ワールド位置
    glm::uvec4 light_count;            // offset 144, 16   x = 有効ライト数
                                       //                  y = 影を落とすライトの添字（無ければ ~0u。D-4）
                                       //                  z, w = 未使用
};

static_assert(sizeof(CameraUBO) == 160, "std140 のレイアウトと一致させること");
```

- ★ **シェーダ側（`triangle.vert` / `triangle.frag` / `shadow.vert`）の `uniform CameraUBO` ブロックを3箇所とも直すこと。** 1つでも忘れると、そのシェーダだけが `camera_position` を `light_view_projection` の一部として読む。**絵は出る**が、視線ベクトルが壊れて鏡面反射だけがおかしくなる。
- ★ 影を落とすライトが無いフレームでは `light_view_projection` に**単位行列ではなく、直前の値か任意の有効な行列**を入れておくこと。単位行列を入れると、シャドウパスが「クリップ空間 = ワールド空間」として描き、意味のない深度が書かれる。どのみち `light_count.y == ~0u` で参照されないが、シャドウパス自体はスキップするのが素直（①-6）。

### ①-5. シャドウキャスタの収集（D-5）

```
// renderer.hpp に足す:
//   std::vector<DrawItem> shadow_items_;
//
// 収集（本パスの収集と同じ view を回すが、判定する視錐台が違う）:
//   const scene::Frustum light_frustum = scene::Frustum::from_view_projection(light_view_projection);
//
//   registry.view<scene::WorldTransform, scene::MeshHandle>().each(...):
//     1. meshes_->contains(mh.id) でなければスキップ
//     2. 境界球をワールドへ移す（既存のコードと同じ。sx/sy/sz の最大値で半径をスケール）
//     3. light_frustum.intersects(...) でなければスキップ
//     4. 半透明マテリアル（scene::Material::transparent）はスキップ（D-5）
//     5. shadow_items_ へ push（distance_sq は使わないので 0 でよい）
//
// ★ 本パスの収集ループと**ほぼ同じコードになる**。1つの view で両方を同時に判定し、
//   2つのリストへ振り分ける形に統合すること（ワールド行列の取り出しと境界球の計算が
//   2回走るのは無駄で、しかも「片方だけ直す」事故の温床になる）。
//
// ソート: メッシュ順（不透明と同じ）。インスタンス化してドローをまとめるため。
//   ★ 半透明のような距離ソートは不要（深度しか書かないので順序に意味が無い）。
//
// instances_ の組み立て（3区間。D-5）:
//   for (opaque)      instances_.push_back(...)
//   for (transparent) instances_.push_back(...)
//   for (shadow)      instances_.push_back(...)
//   first_instance: opaque = 0
//                   transparent = opaque_items_.size()
//                   shadow      = opaque_items_.size() + transparent_items_.size()
//
// ★ kMaxInstances のクランプ処理を3区間ぶんに直すこと。
//   現在は「opaque が超えたら transparent を捨てる」の2段。
//   3段になるので、**shadow_items_ を最初に捨てる**のが素直
//   （影が消えるだけで済み、本体が消えるより被害が小さい）。
```

### ①-6. シャドウパスの記録

```
// record_shadow_pass(command_buffer, frame_index):
//
//   0. 影を落とすライトが無ければ**パスごとスキップ**する。
//      ★ ただしスキップすると、そのフレームのシャドウマップは前フレームの内容のまま
//        （初回は未初期化）。light_count.y == ~0u なので誰も読まないが、
//        バリデーションのために初回だけはクリアしておくこと
//        （= 描くものが無くても BeginRenderPass / EndRenderPass だけは通す方が安全）。
//
//   1. VkClearValue clear{}; clear.depthStencil = { 1.0f, 0 };
//      ★ clearValueCount = 1（カラーが無いので1つだけ）。2 のままにすると
//        「pClearValues の数がアタッチメント数を超えている」で警告が出る。
//
//   2. renderArea = { {0,0}, shadow_maps_[frame]->extent() }
//      framebuffer = shadow_maps_[frame]->framebuffer()
//      renderPass  = shadow_render_pass_->handle()
//
//   3. vkCmdBeginRenderPass
//
//   4. vkCmdSetViewport / vkCmdSetScissor を**シャドウマップの解像度で**設定する。
//      ★ 動的ステートなので、本パスのビューポート（スワップチェーン解像度）とは別に
//        設定し直す必要がある。**忘れると影が画面の左上の一部にしか描かれない**
//        （2048 のマップに 1280x720 のビューポートで描くため）。
//
//   5. vkCmdSetDepthBias(command_buffer, kDepthBiasConstant, 0.0f, kDepthBiasSlope);
//      （①-8 で値を決める）
//
//   6. vkCmdBindPipeline(pipeline_shadow_)
//      vkCmdBindDescriptorSets(layout, set=0, descriptor_sets_[frame])
//      ★ set=1 と set=2 はバインドしない（①-1 の注記）。
//
//   7. record_draw_items(command_buffer, *pipeline_shadow_, shadow_items_,
//                        first_instance = opaque + transparent, instanced = true);
//      ★ 既存の record_draw_items がそのまま使える（メッシュをバインドして
//        vkCmdDrawIndexed するだけなので、パイプラインが何であろうと同じ）。
//
//   8. vkCmdEndRenderPass
```

```
// set=2 のディスクリプタ書き込み（起動時に1回。create_descriptor_sets() の続き、
// または shadow_maps_ の生成直後）:
//
//   for (i = 0; i < kFramesInFlight; ++i):
//     binding=0: { sampler = shadow_sampler_->handle(),
//                  imageView = shadow_maps_[i]->view(),
//                  imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL }
//   ★ imageLayout を SHADER_READ_ONLY_OPTIMAL と書き間違えないこと。
//     深度イメージは DEPTH_STENCIL_READ_ONLY_OPTIMAL。バリデーションが指摘してくれる。
//
// ★ ② を実装するまで binding=1..3 は空のままなので、この時点では**まだ set=2 を
//   バインドしてはいけない**。①だけを先に動かすなら、set=2 のレイアウトを
//   一時的に binding=0 だけにするか、binding=1..3 に既定テクスチャ（白）を
//   書いておくこと。**後者の方がよい**（② に進むときにレイアウトを触り直さずに済む）。
```

### ①-7. シェーダ

**新規 `shaders/shadow.vert`**:

```glsl
#version 450

layout(location = 0) in vec3 in_position;
// ★ normal / uv / tangent は宣言しない（深度しか要らない）。
//   パイプライン側の attribute_descriptions は 4 本のままでよい。
//   「宣言したが使わない location」は許されるが、逆（宣言していないものを読む）は不可。
//   stride（sizeof(Vertex)）を変えないこと。同じ頂点バッファを本パスと共有している。

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4  view_proj;
    mat4  light_view_proj;
    vec4  camera_position;
    uvec4 light_count;
} camera;

struct InstanceData {
    mat4 model;
    uint material_index;
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

void main() {
    gl_Position = camera.light_view_proj
                * instances[gl_InstanceIndex].model
                * vec4(in_position, 1.0);
}
```

- ★ **フラグメントシェーダは作らない**（D-6）。`shaders/` に `shadow.frag` を置かないこと。CMake は `*.vert` / `*.frag` を GLOB しているので、置けば勝手にコンパイルされて紛らわしくなる。
- ★ 新しい `.vert` を足したら **CMake の再実行が要る**（`file(GLOB ...)` は構成時に一度だけ評価される）。「シェーダを足したのに反映されない」の9割はこれ。

**[triangle.frag](../../shaders/triangle.frag) への追加**:

```glsl
layout(set = 2, binding = 0) uniform sampler2DShadow shadow_map;
//   ★ sampler2D ではなく sampler2DShadow。texture() の戻り値が
//     「色」ではなく「比較結果 [0,1]」になり、引数が vec3（uv, 比較する深度）になる。

// world_pos のフラグメントが影の中にいるか。1.0 = 照らされている、0.0 = 完全な影。
float sample_shadow(vec3 world_pos, vec3 N, vec3 L) {
    // 1. 光源クリップ空間へ
    vec4 lp = camera.light_view_proj * vec4(world_pos, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    //   ★ 正射影なので w は常に 1。透視除算は不要だが、書いておくと
    //     phase17 でスポット/点光（透視）に広げたときそのまま動く。

    // 2. NDC -> テクスチャ座標
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float z = ndc.z;
    //   ★ z は再マップしない。GLM_FORCE_DEPTH_ZERO_TO_ONE により既に [0,1]。
    //     OpenGL 由来の記事にある `* 0.5 + 0.5` をここにも書くと、
    //     全フラグメントが「手前」と判定されて**影が完全に消える**。
    //   ★ uv.y も反転しない。シャドウパスと本パスで同じ向きのビューポート
    //     （y = 0, height = +H）を使っているので、そのまま一致する。
    //     反転させると**影が上下逆の位置に出る**（物体の反対側に影が付く）。

    // 3. ライトの far より遠いフラグメントは影にしない
    if (z > 1.0) { return 1.0; }
    //   ★ uv が [0,1] の外に出た場合は CLAMP_TO_BORDER + 白ボーダーが
    //     自動的に 1.0 を返すので、明示の範囲チェックは要らない（⓪-3）。

    // 4. 法線オフセット（①-8）
    //    受光面が斜めなほど大きくずらす。depth bias と併用する。
    float bias = max(kNormalBiasMax * (1.0 - dot(N, L)), kNormalBiasMin);

    // 5. 3x3 PCF
    //    texelSize = 1.0 / textureSize(shadow_map, 0)
    //    for (y = -1..1) for (x = -1..1)
    //        sum += texture(shadow_map, vec3(uv + vec2(x, y) * texelSize, z - bias));
    //    return sum / 9.0;
    //    ★ sampler2DShadow なので texture() の戻り値が既に比較結果。
    //      さらにハードウェアが 2x2 のバイリニア補間を掛けるので、
    //      3x3 の手動ループと合わせて実質 6x6 相当の平滑になる。
    return 0.0;  // TODO(①-7)
}
```

- ★ `kNormalBiasMax` / `kNormalBiasMin` は NDC の z 空間での値。`kShadowFar - kShadowNear` が 200 なので、**ワールド単位の「5cm」は z 空間では 0.00025**。桁を間違えると「影が全く出ない（大きすぎ）」か「縞模様（小さすぎ）」になる。0.0005 / 0.00005 あたりから始める。

### ①-8. depth bias と PCF の調整

**シャドウアクネ**（自己遮蔽による縞模様）の対策は3つあり、**併用する**。

| 対策 | 何をするか | 副作用 |
|---|---|---|
| **depth bias**（`vkCmdSetDepthBias`） | シャドウマップに書く深度を、面の傾きに応じて奥へずらす | 大きすぎると peter-panning |
| **法線オフセット**（シェーダ側） | 比較する深度を、法線方向へずらしたぶんだけ引く | 同上 |
| **前面カリング**（`VK_CULL_MODE_FRONT_BIT`） | シャドウマップに「裏面」を書く。表面はその手前になるので自己遮蔽しない | **薄い板が影を落とさなくなる**（表も裏も同じ位置なので） |

```
// 出発点の値（2048 × 正射影半幅 40 の場合）:
//   kDepthBiasConstant = 1.25f
//   kDepthBiasSlope    = 1.75f
//   kNormalBiasMax     = 0.0005f
//   kNormalBiasMin     = 0.00005f
//
// ★ 調整の順序:
//   1. まず bias を全部 0 にして描く。**縞模様が出るのが正常**（出ないなら
//      影がそもそも出ていない。式ではなく配管を疑うこと）
//   2. slope だけ上げて縞を消す
//   3. 影が物体から離れて見える（peter-panning）なら constant を下げる
//   4. それでも足りなければ解像度を上げる（2048 → 4096）か、正射影の箱を小さくする
//      ★ 実は**箱を小さくするのが一番効く**。40 → 20 にすればテクセル密度が4倍になる。
//        「解像度を上げる」より先に「範囲を狭める」を試すこと。
```

**peter-panning**（影が物体から浮いて離れる）は「bias が大きすぎる」か「前面カリングで薄い物体が抜けた」のどちらか。前者は値を下げる、後者は `cull_mode` を `NONE` に戻して bias で対処する。

### ①-9. 検証シーン（[main.cpp](../../sandbox_graphics/main.cpp)）

```
// 1. 影が落ちる「床」を用意する。
//    add_plane_mesh() を大きくスケール（例: 100 × 1 × 100）して原点に置く。
//    ★ 現在の検証シーンには床が無い。床が無いと影の落ちる先が他のモデルしか
//      無く、「影が出ていない」のか「影を受ける面が見えていない」のか分からない。
//    ★ マテリアルは roughness = 0.9 / metallic = 0.0 の無地にすること。
//      鏡面が強いと影のコントラストが読みにくい。
//
// 2. 方向光に cast_shadows = true を立てる。
//    既存の create_directional_light に引数を1本足す。
//
// 3. 点光源の数を一時的に減らす（4灯程度）。
//    ★ 20灯のままだと、影の中も他のライトで明るく照らされて
//      「影が薄い」のか「影が出ていない」のか区別できない。
//      配管が通ってから元に戻すこと。
//
// 4. 光源を時間で回す。
//    方向光の Transform を毎フレーム回転させる（Y 軸周り、0.2 rad/s 程度）。
//    ★ これが**接線バグのときと同じ確認方法**。影が「太陽と反対側に」
//      「物体の高さに比例した長さで」伸びれば、行列と符号は正しい。
//      逆向きに伸びるなら light_dir の符号、伸びる長さが変なら正射影の範囲。
```

### ①-10. 検証

- 床に影が落ち、**太陽と反対側へ**伸びること。
- 光源を回すと影が**滑らかに追随**すること（飛ぶ・ちらつくなら ⓪-1 のサブパス依存 [1] を疑う）。
- 影の縁が PCF で滑らかなこと（1テクセルのギザギザなら PCF が効いていない）。
- カメラを回して**画面外の物体の影が画面内に残る**こと（D-5 が効いている証拠）。
- 正射影の箱の外（原点から 40 以上離れた場所）では影が出ないこと。これは**仕様どおり**。
- ライトを 65 灯に戻しても影が1つだけ正しく出ること。
- リサイズ・F11 で影が壊れないこと（`shadow_maps_` を作り直していないことの確認）。
- 終了時にバリデーションのリーク報告が無いこと。

### ①-11. よくある壊れ方の一覧

| 症状 | 最初に疑うところ |
|---|---|
| 画面全体が縞模様（シャドウアクネ） | depth bias が 0（①-8。**最初は必ずこれが出る**） |
| 影が物体から離れて浮く（peter-panning） | bias が大きすぎる / 前面カリングと薄い板 |
| 影が**全く**出ない | `light_count.y` が `~0u` のまま（`cast_shadows` を立て忘れ）／ set=2 をバインドしていない／ frag の `z` に `*0.5+0.5` を足している |
| 影が**画面全体**に出る（全部真っ暗） | 比較サンプラの `compare_op` が `GREATER` 系／ `light_view_projection` が NaN（`lookAt` の up 縮退） |
| 影がシーンにタイル状に繰り返す | シャドウサンプラの `addressMode` が `REPEAT`（⓪-3） |
| 影が上下反転した位置に出る | frag で `uv.y` を反転している |
| 影がノイズ・砂嵐になる | シャドウレンダーパスの `depth_store_op` が `DONT_CARE`（⓪-1） |
| 影が画面の左上 1/4 にしか描かれない | シャドウパスで `vkCmdSetViewport` をしていない（①-6 手順4） |
| 影がちらつく・1フレーム遅れる | ⓪-1 のサブパス依存 [1]（LATE_FRAGMENT_TESTS → FRAGMENT_SHADER）が無い |
| リサイズすると影が消える | `recreate_swapchain` で `shadow_maps_` を作り直している |
| 鏡面反射だけがおかしい | `CameraUBO` の拡張をシェーダ3本のうち一部でしか反映していない（①-4） |
| バリデーション「descriptor image layout mismatch」 | シャドウパス中に set=2 をバインドしている（①-1） |

---

## ② IBL（イメージベースドライティング）

**このステップのゴールは phase15 D-8 の宿題を返すこと**: `metallic = 1` のマテリアルが真っ黒でなくなり、周囲の景色が映り込む。

### ②-1. `Cubemap` クラス（新規 `cubemap.hpp` / `.cpp`）

```cpp
// キューブマップ（6面のレイヤを持つ VkImage）の RAII ラッパー（phase16 ②-1 / D-8）。
//
// Texture（[texture.hpp](../../engine/include/sq/graphics/texture.hpp)）とは分ける。
//   Texture は「ファイル／ピクセル列から作って SHADER_READ_ONLY で固定」だが、
//   Cubemap は「コンピュートが GENERAL レイアウトで書き込み、書き終えたら
//   SHADER_READ_ONLY へ遷移する」という別の寿命を持つ。
class Cubemap {
public:
    // size:   1面の一辺（正方形）
    // mips:   mip レベル数（prefiltered specular で >1 になる）
    // format: VK_FORMAT_R16G16B16A16_SFLOAT 固定でよいが、引数で受けておく（D-8）
    //
    // ★ VkImageCreateInfo に必要なもの（どれか1つでも抜けると動かない）:
    //     arrayLayers = 6
    //     flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    //     usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    Cubemap(VkDevice device, GpuAllocator& allocator,
            std::uint32_t size, std::uint32_t mips, VkFormat format);
    ~Cubemap();

    Cubemap(const Cubemap&) = delete;
    Cubemap& operator=(const Cubemap&) = delete;

    // サンプル用ビュー（VK_IMAGE_VIEW_TYPE_CUBE, 全 mip, layerCount = 6）。
    // set=2 の binding=1 / binding=2 に書く。
    [[nodiscard]] VkImageView sample_view() const;

    // 書き込み用ビュー（VK_IMAGE_VIEW_TYPE_2D_ARRAY, 単一 mip, layerCount = 6）。
    //
    // ★ mip ごとに別のビューが要る。imageStore は mip レベルを指定できないため、
    //   「どの mip に書くか」はビューが決める（D-8）。コンストラクタで mips 本ぶん
    //   まとめて作っておくこと。
    [[nodiscard]] VkImageView storage_view(std::uint32_t mip) const;

    [[nodiscard]] VkImage handle() const;   // レイアウト遷移に使う
    [[nodiscard]] std::uint32_t size() const;
    [[nodiscard]] std::uint32_t mips() const;

private:
    GpuAllocator* allocator_ = nullptr;
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView sample_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> storage_views_;  // mip ごと
    std::uint32_t size_ = 0;
    std::uint32_t mips_ = 1;
};
```

- ★ **レイアウト遷移のヘルパが要る**。`Texture::transition_image_layout` は private な static だが、キューブ（layerCount = 6、mip 複数）にも使いたい。**`image_utils.hpp` のようなフリー関数へ括り出す**こと。`Texture` 側もそちらを呼ぶように直す。
  - 必要になる遷移: `UNDEFINED → GENERAL`（書き込み前）、`GENERAL → SHADER_READ_ONLY_OPTIMAL`（書き込み後）。
  - phase15 までに実装済みの遷移パターンとは**別のパターン**なので、`old_layout` / `new_layout` の分岐に枝が増える。

### ②-2. HDR 画像の読み込み

```cpp
// texture.hpp に float 版のコンストラクタを足す、または新しいクラスにする。
//
// stb_image の stbi_loadf が float* を返す（1ピクセル 4 × float = 16 バイト）。
// これを VK_FORMAT_R32G32B32A32_SFLOAT の 2D テクスチャとして GPU へ上げる。
//
// ★ ステージングバッファのサイズが width * height * 4 * sizeof(float)。
//   2048 × 1024 の HDR なら 32 MiB。既存の Texture は
//   width * height * 4（バイト）で計算しているので、**そのまま流用すると 1/4 しか
//   転送されず、下 3/4 が真っ黒になる**。
//
// ★ mip は不要（equirect → cube の変換で1回読むだけ）。mip_levels = 1 にすること。
// ★ 変換が終わったら**この 2D テクスチャは破棄してよい**（キューブができれば用済み）。
//   起動時に 32 MiB を確保して即座に解放する形になる。
```

- 検証用の HDR は Poly Haven などの CC0 素材を `assets/textures/env/` に置く。`assets/textures` は既に実行ファイルの隣へコピーされるので**CMake の変更は不要**。

### ②-3. `ComputePipeline` クラス（新規 `compute_pipeline.hpp` / `.cpp`）

```cpp
// コンピュートパイプラインの RAII ラッパー（phase16 ②-3 / D-7。このエンジンで初）。
//
// GraphicsPipeline に比べて劇的に短い。要るのは:
//   - シェーダモジュール1本（VK_SHADER_STAGE_COMPUTE_BIT）
//   - パイプラインレイアウト（ディスクリプタセットレイアウト + プッシュ定数）
//   - vkCreateComputePipelines
// ラスタライズ状態・ビューポート・ブレンド・深度・頂点入力が**すべて無い**。
class ComputePipeline {
public:
    // push_constant_size: 0 なら pushConstantRangeCount = 0。
    //   ★ ここではプッシュ定数を使う（D-3 で本パスに使わないと決めたのとは別の話）。
    //     prefiltered specular が「今どの mip を焼いているか（= roughness）」を
    //     渡すのに要る。mip ごとにディスクリプタセットを作り直すより素直。
    ComputePipeline(VkDevice device, const std::string& comp_spv_path,
                    const std::vector<VkDescriptorSetLayout>& set_layouts,
                    std::uint32_t push_constant_size);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const;
    [[nodiscard]] VkPipelineLayout layout() const;

private:
    // ★ GraphicsPipeline::load_shader_module と同じ処理。
    //   **フリー関数 load_shader_module(device, path) へ括り出して共有すること。**
    //   コピーすると、片方だけ直す事故が起きる（phase15 で何度も踏んだパターン）。
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
```

```
// CMake の変更（sandbox_graphics/CMakeLists.txt）:
//   file(GLOB SHADERS ...) に ${SHADER_SOURCE_DIR}/*.comp を足す。
//   ★ 足したうえで CMake を再実行すること（GLOB は構成時にしか評価されない）。
```

```
// physical_device.cpp の変更:
//   find_queue_families で graphics_family を選ぶとき、
//   VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT の両方を要求する（D-7）。
//   ★ 実質すべての GPU で同じファミリが両方を持つが、要求しておかないと
//     「たまたま動いていた」状態になる。
```

### ②-4. equirectangular → キューブへの変換（`shaders/equirect_to_cube.comp`）

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D equirect;      // 入力（HDR の 2D）
layout(set = 0, binding = 1, rgba16f) uniform writeonly imageCube out_cube;
//   ★ imageCube でも image2DArray でもよいが、**フォーマット修飾子（rgba16f）が必須**。
//     書き込み専用なら writeonly を付ける（読まないことを明示するとドライバが最適化できる）。

// face（0..5）と面内の [-1,1] 座標から、キューブの表面上の方向ベクトルを作る。
// 面の順序は Vulkan/D3D の規約: +X, -X, +Y, -Y, +Z, -Z
// ★ ここを間違えると「空が上下逆」「左右が入れ替わる」という形で出る。
//   スカイボックス（②-9）を先に動かして目で確かめてから、irradiance に進むこと。
vec3 cube_direction(uint face, vec2 st);

void main() {
    // uv   = (gl_GlobalInvocationID.xy + 0.5) / size
    // st   = uv * 2.0 - 1.0
    // dir  = normalize(cube_direction(gl_GlobalInvocationID.z, st))
    //
    // equirect の UV:
    //   u = atan(dir.z, dir.x) / (2 * PI) + 0.5
    //   v = acos(clamp(dir.y, -1.0, 1.0)) / PI
    //   ★ v の向き（上下）は画像の原点の取り方で変わる。空が地面に来たら
    //     v を 1.0 - v にする。**これは目で見れば一瞬で分かる**ので、
    //     ②-9 のスカイボックスを先に通しておく意味がここにある。
    //
    // imageStore(out_cube, ivec3(gl_GlobalInvocationID), texture(equirect, vec2(u, v)));
}
```

- ★ ディスパッチ: `vkCmdDispatch(cb, size / 8, size / 8, 6)`。`size` は 8 の倍数にすること（512 など）。8 の倍数でないと端のテクセルが書かれない（`imageStore` は範囲外を無視するので**落ちずに黒い帯が出る**）。

### ②-5. irradiance map（`shaders/irradiance.comp`）

```
// 出力: 32 × 32 のキューブ、mip なし。
//   ★ 小さくてよい。拡散の畳み込みは極端に低周波なので、32 で十分。
//     128 にしても絵はほぼ変わらず、前計算だけが 16 倍遅くなる。
//
// 各出力テクセルについて:
//   N = cube_direction(face, st)
//   N を軸とする接空間を作り、半球を緯度・経度で走査（刻み 0.025 rad 程度）:
//     irradiance += texture(env_cube, sample_dir).rgb * cos(theta) * sin(theta)
//   irradiance = PI * irradiance / sample_count
//
// ★ sin(theta) は立体角の重み、cos(theta) はランバートの入射角。**両方要る**。
//   cos だけだと極が明るくなりすぎ、sin だけだと全体が平坦になる。
// ★ 入力は「元の環境キューブ」であって、prefiltered ではない。
```

### ②-6. prefiltered specular map（`shaders/prefilter.comp`）

```
// 出力: 128 × 128 のキューブ、mip 5 枚。
//   mip 0 = roughness 0.00（完全な鏡）
//   mip 1 = roughness 0.25
//   mip 2 = roughness 0.50
//   mip 3 = roughness 0.75
//   mip 4 = roughness 1.00
//   roughness = float(mip) / float(mips - 1)
//
// ★ mip ごとに vkCmdDispatch を呼ぶ（5 回）。
//   - 出力ビューは cubemap.storage_view(mip)（②-1）
//   - roughness はプッシュ定数で渡す（②-3）
//   - ディスパッチのサイズも mip ごとに半分になる: max(128 >> mip, 1) / 8
//     ★ mip 4 は 8 × 8 なので、local_size 8 のときディスパッチは (1, 1, 6)。
//       割り算で 0 になる mip があると**その mip が一切書かれず、
//       roughness 1.0 の金属だけが真っ黒**になる。max(..., 1) を忘れないこと。
//
// 各出力テクセルについて（GGX 重点サンプリング、1024 サンプル）:
//   N = R = V = cube_direction(face, st)    ← 視線 = 法線 という近似
//   for (i = 0; i < 1024; ++i):
//     Xi = hammersley(i, 1024)
//     H  = importance_sample_ggx(Xi, N, roughness)
//     L  = normalize(2 * dot(V, H) * H - V)
//     if (dot(N, L) > 0):
//        color  += texture(env_cube, L).rgb * dot(N, L)
//        weight += dot(N, L)
//   result = color / weight
//
// ★ 「視線 = 法線」の近似のせいで、斜めから見た伸びたハイライト（anisotropic streak）は
//   再現されない。これは**近似の限界であってバグではない**（phase15 D-8 の
//   「metallic が黒い」と同じで、知っていれば悩まない類の話）。
```

### ②-7. BRDF LUT（`shaders/brdf_lut.comp`）

```
// 出力: 512 × 512 の 2D、VK_FORMAT_R16G16_SFLOAT。
//   x 軸 = NdotV [0,1]、y 軸 = roughness [0,1]
//   出力 = (scale, bias)。specular = prefiltered * (F0 * scale + bias)
//
// ★ キューブではないので Cubemap クラスは使わない。
//   「ストレージイメージとして書ける 2D テクスチャ」が要る。
//   Texture クラスに usage を渡せるようにするか、専用の小さいクラスを作ること。
//
// ★ 環境マップに依存しない**完全に固定の表**。
//   本来はファイルに焼いて同梱できる（起動が速くなる）が、
//   学習目的なので毎回計算する。512×512 × 1024 サンプルでも数十 ms。
//
// ★ ここだけ Smith の k が違う:
//     直接照明（triangle.frag の G_Smith）: k = (roughness + 1)^2 / 8
//     IBL（この LUT）:                      k = roughness^2 / 2
//   **同じ式を流用すると、粗い面の鏡面反射が暗くなりすぎる。**
//   phase15 で書いた G_Smith をコピーしてくると必ず踏む。
```

### ②-8. フラグメントシェーダでの合成（[triangle.frag](../../shaders/triangle.frag)）

```glsl
layout(set = 2, binding = 1) uniform samplerCube irradiance_map;
layout(set = 2, binding = 2) uniform samplerCube prefiltered_map;
layout(set = 2, binding = 3) uniform sampler2D   brdf_lut;

// IBL 用のフレネル。**通常の F_Schlick とは別物**。
//   粗い面ほど縁の反射を抑える。通常版を使うと、roughness 1.0 の面の輪郭が
//   不自然に白く光る（「縁取りされたように見える」）。
vec3 F_SchlickRoughness(float NdotV, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}
```

```glsl
// main() の変更点。
//
// 削除:
//   const float kAmbient = 0.03;
//   vec3 result = kAmbient * albedo * ao;
//   ★ 消し忘れると環境光が**二重に**乗って全体が白っぽくなる。
//     しかも「それらしい絵」なので気付きにくい。**消すこと。**
//
// 追加（ライトのループの後）:
//   float NdotV_ibl = max(dot(N, V), 0.0);
//   vec3  F_ibl  = F_SchlickRoughness(NdotV_ibl, F0, roughness);
//   vec3  kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
//
//   // 拡散
//   vec3 irradiance  = texture(irradiance_map, N).rgb;
//   vec3 diffuse_ibl = kD_ibl * albedo * irradiance;
//
//   // 鏡面
//   vec3  R = reflect(-V, N);
//   const float kMaxReflectionLod = 4.0;   // ★ prefiltered の mip 数 - 1 と一致させること
//   vec3  prefiltered = textureLod(prefiltered_map, R, roughness * kMaxReflectionLod).rgb;
//   vec2  ab = texture(brdf_lut, vec2(NdotV_ibl, roughness)).rg;
//   vec3  specular_ibl = prefiltered * (F_ibl * ab.x + ab.y);
//
//   vec3 ambient = (diffuse_ibl + specular_ibl) * ao;
//   result = ambient + <ライトのループの累積>;
//
// ★ kMaxReflectionLod をハードコードするなら、C++ 側の mip 数と一致しているかを
//   コメントで明示すること。ずれると「roughness 1.0 だけ映り込みが鋭い」という
//   気付きにくい壊れ方になる。textureQueryLevels(prefiltered_map) - 1 で
//   シェーダ側から取れるが、定数の方が速い。
//
// ★ トーンマップ（result = result / (result + 1)）は**今までどおり最後に置く**。
//   IBL の寄与を足した後でないと、明るい部分が飽和する。
```

### ②-9. スカイボックス（環境マップの可視化）

**先に作ること。** キューブの面順序・上下反転・色空間の間違いは、irradiance や prefiltered を見ても分からない（どちらもぼけているため）。**背景に環境マップをそのまま出せば、1秒で分かる。**

```
// 方式: フルスクリーン三角形 + inverse(view_projection) でレイ方向を作る。
//
// 立方体メッシュを描く定番手法を採らない理由:
//   - 頂点バッファもインデックスバッファも要らない
//   - メッシュレジストリに「スカイボックス専用の立方体」を登録しなくてよい
//   - カリング方向（内側を見る）・立方体のサイズ・near/far との干渉を考えなくてよい
//
// 新規: shaders/skybox.vert / shaders/skybox.frag
//
// skybox.vert:
//   頂点属性を持たない（PipelineConfig::has_vertex_input = false）。
//   gl_VertexIndex（0,1,2）から画面を覆う大きな三角形の NDC 座標を作る:
//     vec2 uv  = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
//     vec4 pos = vec4(uv * 2.0 - 1.0, 1.0, 1.0);   // ★ z = 1.0（最奥）
//     gl_Position = pos;
//   レイ方向は逆行列で求める:
//     mat4 inv = inverse(camera.view_proj);   // ★ 頂点3つぶんしか走らないので許容
//     vec4 w   = inv * pos;
//     out_dir  = w.xyz / w.w - camera.camera_position.xyz;
//   ★ frag 側で normalize すること（補間でスケールが崩れる）。
//
// skybox.frag:
//   out_color = vec4(texture(env_cube, normalize(in_dir)).rgb, 1.0);
//   ★ トーンマップを掛けること（HDR をそのまま出すと白飛びする）。
//     本パスと同じ result / (result + 1) を使う。
//
// パイプライン:
//   PipelineConfig{ .depth_write_enable = false,           // 背景なので深度を書かない
//                   .blend_enable = false,
//                   .has_color_attachment = true,
//                   .depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL,  // ★ z = 1.0 で描くため
//                   .has_vertex_input = false }
//
// 描画位置: **不透明パスの後・半透明パスの前**。
//   ★ 不透明より先に描くと、不透明に上書きされるだけなので無駄
//     （深度テストで弾かれるぶん、後に描いた方が速い）。
//   ★ 半透明より後に描くと、半透明の向こうに背景が出ない。
//   vkCmdDraw(command_buffer, 3, 1, 0, 0);   // 頂点3つ、インデックス無し
//
// set=2 に環境キューブ本体の binding を足すか迷うところだが、
// **binding=2（prefiltered）の mip 0 で代用してよい**（roughness 0 = 元の環境そのもの）。
//   ★ ただし prefiltered は 128×128 なので背景としては粗い。
//     綺麗に出したいなら set=2 に binding=4 として元の環境キューブ（512）を足すこと。
//     デバッグ用途なら 128 で足りる。
```

### ②-10. 検証

- **スカイボックスが正しく出ること**（これが他のすべての前提）。
  - 空が上、地面が下（逆なら equirect の `v`。②-4）
  - カメラを一周させて景色が連続していること（継ぎ目があれば面の順序。②-4）
  - カメラを動かしても背景が動かないこと（動くなら `w.xyz / w.w` の平行移動を引き忘れ）
- **`metallic = 1` のマテリアルボールが黒くなくなること**（phase15 D-8 の宿題）。
- 金属球への**映り込みが背景と一致**すること。ずれていたら `reflect(-V, N)` の符号。
- `roughness` を 0 → 1 と上げると映り込みが**段階的に**ぼけること。
  - 段階が5段に見えるのは `kMaxReflectionLod` の mip が5枚だから。`textureLod` のレベル間補間が効いていれば滑らかになる。**カクついたら mip の書き込みが抜けている**（②-6 の `max(..., 1)`）。
- 誘電体（`metallic = 0`, `roughness = 1`）が**環境の平均色**で薄く照らされること。
- `occlusion` テクスチャを持つモデル（`DamagedHelmet.glb` 等）で、くぼみが暗くなること。
- ★ 全体が白っぽく飛んでいたら `kAmbient` の消し忘れ（②-8）。
- ★ 粗い面の輪郭が白く光っていたら `F_Schlick` と `F_SchlickRoughness` の取り違え（②-8）。
- ★ 粗い金属が暗すぎたら BRDF LUT の `k` が直接照明用のまま（②-7）。

---

## ③ `AssetRegistry<T>` と参照カウント

**絵は1ピクセルも変わらない。** ①② と独立なので、いつ着手してもよい。

### ③-1. 3つのレジストリの共通項と差分

| | `MeshRegistry` | `TextureRegistry` | `MaterialRegistry` |
|---|---|---|---|
| `Slot { entry, generation, alive }` | 同じ | 同じ | 同じ |
| `free_indices_` の再利用 | 同じ | 同じ | 同じ |
| `contains()`（範囲 + 世代照合） | 同じ | 同じ | 同じ |
| `get()` | 同じ | （無い） | （無い） |
| 上限チェック | 無し | `max_textures_` | `max_materials_` |
| **登録時の副作用** | 無し | bindless 配列へ書き込み | SSBO の要素へ書き込み |
| **解放時の処理** | DeletionQueue へ | DeletionQueue へ + bindless を既定で埋め直す + `by_path_` から消す | 即時（枠を空けるだけ） |
| Entry の型 | `{ vb, ib, bounds }` | `{ unique_ptr<Texture> }` | `MaterialData` |

→ **差分は「登録時の副作用」と「解放時の処理」の2つだけ**。仮想関数2本で吸収できる（D-10）。

### ③-2. `AssetRegistry<Handle, Entry>`（新規 `engine/include/sq/graphics/asset_registry.hpp`、ヘッダオンリー）

```cpp
// 3つのアセットレジストリの共通部分（phase16 ③-2 / D-10）。
//
// phase14 ②-2 のコメント（material_registry.hpp）に書いたとおり、
// 「3つとも書いてみてから共通項を抜く」という順序でここへ来た。
//
// Handle: scene::MeshId / TextureId / MaterialId（AssetHandle の派生）
// Entry:  スロットに置く実体。**ムーブ可能であればよい**（コピー不可でよい）。
template <typename Handle, typename Entry>
class AssetRegistry {
public:
    virtual ~AssetRegistry() = default;

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // 範囲 + 世代の照合（phase14 ②。ABA 問題の検出）。
    [[nodiscard]] bool contains(Handle id) const;

    // 範囲外／世代不一致は例外（先に contains で確認する）。
    [[nodiscard]] const Entry& get(Handle id) const;

    // -- 参照カウント（③-4） --
    void retain(Handle id);
    // 参照カウントを1減らし、0 になったら実体を解放する。
    void release(Handle id);
    [[nodiscard]] std::uint32_t ref_count(Handle id) const;

    // 生きているスロット数（検証・デバッグ用。③-7）。
    [[nodiscard]] std::size_t alive_count() const;

protected:
    // capacity: 0 なら無制限（MeshRegistry）。
    explicit AssetRegistry(std::uint32_t capacity = 0);

    struct Slot {
        Entry entry{};
        std::uint32_t generation = 0;
        std::uint32_t ref_count = 0;   // ★ phase16 ③-4 で追加
        bool alive = false;
    };

    // スロットを確保して Handle を払い出す（派生の add() が最後に呼ぶ）。
    //   1. free_indices_ が空でなければ末尾から取り出して再利用、空なら push_back
    //   2. generation はインクリメントしない（上げるのは解放時）
    //   3. alive = true, ref_count = 0
    //   4. on_registered(index, entry) を呼ぶ
    //   ★ capacity_ を超えたら例外を投げること（現在は TextureRegistry / MaterialRegistry が
    //     各自で判定している。ここへ集約する）。
    [[nodiscard]] Handle emplace(Entry entry);

    // 参照カウントを無視して即座に解放する（既存の unload() 相当）。
    //   1. contains(id) でなければ何もしない（二重解放の防止）
    //   2. on_destroy(index, std::move(slot.entry)) を呼ぶ
    //   3. alive = false, ++generation, ref_count = 0
    //   4. free_indices_.push_back(index)
    void destroy(Handle id);

    // -- 派生が埋めるフック --

    // 登録直後に呼ばれる。bindless 配列や SSBO への書き込みをここでやる。
    virtual void on_registered(std::uint32_t index, const Entry& entry) {}

    // 解放時に呼ばれる。entry の所有権が渡る。
    //   MeshRegistry / TextureRegistry は DeletionQueue へ積む。
    //   MaterialRegistry は何もしない（枠を空けるだけ）。
    virtual void on_destroy(std::uint32_t index, Entry&& entry) = 0;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
    std::uint32_t capacity_ = 0;
};
```

- ★ **`Entry` はムーブのみでよい**が、`std::vector<Slot>` の再確保でムーブが走るので `Slot` はムーブ可能でなければならない。`MeshRegistry::Entry` は `unique_ptr` 2本なので自動的にムーブのみになる。**コピーを要求する書き方をしないこと**（`slots_.resize(n)` してから代入する形は、型によってはコピーを呼ぶ）。
- ★ **仮想関数をコンストラクタ／デストラクタから呼ばないこと。** `on_destroy` は派生クラスの状態（`deletions_` 等）を使うので、基底のデストラクタから呼ぶと派生部分が既に壊れている。**各派生クラスのデストラクタで明示的に全スロットを片付ける**こと（現在の3つのデストラクタが既にそうなっている）。
- ★ `contains` / `get` / `retain` / `release` は `Handle` を値で受ける（8 バイトなので参照にする意味が無い）。

### ③-3. 3つのレジストリを派生させる

```cpp
// mesh_registry.hpp
class MeshRegistry : public AssetRegistry<scene::MeshId, MeshRegistry::Entry> { ... };
//   ★ 自分の入れ子型を基底のテンプレート引数に使えないので、Entry は
//     名前空間スコープの struct（例: MeshEntry）へ出すこと。
//     TextureRegistry / MaterialRegistry も同様。

// 各派生が実装するもの:
//   MeshRegistry::on_destroy
//     → shared_ptr へ載せ替えて deletions_->push([b = std::move(buffers)]{ ... })
//       （deletion_queue.hpp の注記 (a) の方法）
//
//   TextureRegistry::on_registered
//     → bindless 配列の index 番目へ vkUpdateDescriptorSets
//   TextureRegistry::on_destroy
//     → 1. bindless 配列の index 番目を**既定テクスチャの view で上書き**
//          （PARTIALLY_BOUND は「一度も書いていない要素」しか救わない。phase14 ②-3）
//       2. by_path_ から該当エントリを消す（★ 消し忘れると次の load が死んだ
//          ハンドルを返す）
//       3. DeletionQueue へ積む
//
//   MaterialRegistry::on_registered
//     → buffer_->update(index, data)
//   MaterialRegistry::on_destroy
//     → そのマテリアルが持つテクスチャの AssetRef を落とす（③-4 の連鎖解放）
//       ★ Slot に AssetRef を持たせれば、Entry のムーブ／破棄だけで自動的に走る。
//         明示的に書く必要は無い。
```

### ③-4. 参照カウントと `AssetRef`（新規 `engine/include/sq/scene/asset_ref.hpp`）

```cpp
// アセットへの所有参照（phase16 ③-4 / D-9）。
//
// コピーすると +1、破棄すると -1。0 になった時点でレジストリが実体を解放する。
// shared_ptr との違いは「カウントがレジストリのスロット側にある」こと
// （ハンドルは 8 バイトのままで、制御ブロックを別途確保しない）。
template <typename Registry, typename Handle>
class AssetRef {
public:
    AssetRef() = default;

    // ★ このコンストラクタが retain する。
    //   レジストリの add() は生の Handle を返すので、
    //   受け取った側がここへ包んだ時点で参照カウントが 0 → 1 になる。
    AssetRef(Registry& registry, Handle id);

    AssetRef(const AssetRef& other);              // retain
    AssetRef(AssetRef&& other) noexcept;          // 移譲（retain しない。other を空にする）
    ~AssetRef();                                  // release

    AssetRef& operator=(const AssetRef& other);   // ★ 自己代入を必ず扱うこと
    AssetRef& operator=(AssetRef&& other) noexcept;

    // GPU へ渡す生の添字はここから取る。
    [[nodiscard]] Handle id() const { return id_; }
    [[nodiscard]] bool valid() const { return registry_ != nullptr && !id_.is_null(); }

    void reset();   // release して空にする

private:
    Registry* registry_ = nullptr;   // 所有しない
    Handle id_{};
};

// 別名（呼び出し側の見た目を短くする）:
//   using MeshRef     = AssetRef<graphics::MeshRegistry,     MeshId>;
//   using TextureRef  = AssetRef<graphics::TextureRegistry,  TextureId>;
//   using MaterialRef = AssetRef<graphics::MaterialRegistry, MaterialId>;
```

- ★ **自己代入とムーブ後の空状態**を必ず扱うこと。`a = a;` で release → retain の順に走ると、カウント 1 の状態で解放されて**ダングリングになる**。先に retain してから release するか、同一なら早期 return。
- ★ **`AssetRef` はレジストリより長生きしてはいけない。** Renderer のデストラクタでレジストリが死んだ後に `AssetRef` が破棄されると、死んだポインタへ `release()` を呼ぶ。`main.cpp` の `LoadedModel` は `Renderer` より**後に宣言されている**か確認すること（宣言順 = 構築順、破棄は逆順）。
  - ★ これは phase14 で `materials_` を `textures_` より先に破棄すると決めたのと同じ種類の話。**このフェーズで一番踏みやすい罠。**

**`by_path_` のキャッシュヒットとの噛み合わせ**:

```
// TextureRegistry::load が「既に登録済み」と判定して既存の ID を返すとき、
// **参照カウントは呼び出し側が AssetRef に包んだ時点で +1 される**。
// つまり同じテクスチャを2回 load すれば ref_count は 2 になる。
//
// ★ ここを取り違えて「キャッシュヒットなら retain しない」と書くと、
//   2体目のモデルを破棄した瞬間に1体目のテクスチャが消える
//   （絵は市松模様のフォールバックになる。D-9 の安全網が効くので落ちはしない）。
// ★ 逆に load 自身が内部で retain までしてしまうと、
//   「add が返した直後の ref_count が 1」と「AssetRef に包んで 2」で二重になる。
//   **retain するのは AssetRef のコンストラクタだけ**、と一本に決めること
//   （phase15 ③-9 バグA と同じ教訓: 中立値／副作用の表を2箇所に置かない）。
```

### ③-5. 既定アセットの永続参照

```
// 起動時に登録する既定アセットは、**誰も参照しなくなっても解放してはいけない**:
//   textures/default.png（市松模様）
//   白 1×1（create_white_texture）
//   フラット法線 1×1（create_flat_normal_texture）
//   既定マテリアル（materials_->add(MaterialData{}, ...)）
//   （将来）既定メッシュ
//
// 対処: Renderer がこれらの AssetRef をメンバとして持つ。
//   scene::TextureRef default_texture_ref_;
//   scene::TextureRef white_texture_ref_;
//   scene::TextureRef flat_normal_texture_ref_;
//   scene::MaterialRef default_material_ref_;
//
// ★ 忘れると「最後のモデルを unload した瞬間に既定テクスチャが消え、
//   以後すべてのフォールバックが死ぬ」。しかも最初のモデルをロードしている間は
//   正常に見えるので、**解放を実装するまで発覚しない。**
// ★ これらの AssetRef は、各レジストリより**後に破棄**されてはならない。
//   Renderer のメンバ宣言順を、レジストリより**前**にすること
//   （破棄は宣言の逆順なので、AssetRef が先に死ぬ）。
```

### ③-6. モデルのアンロード

```cpp
// gltf_loader.hpp の LoadedModel を、生ハンドルから AssetRef 群へ変える。

struct LoadedModel {
    struct Node { /* ... 変更なし（生ハンドルのまま。展開時に ECS へコピーする） ... */ };
    std::vector<Node> nodes;

    // ★ phase16 ③-6 で追加。このファイルがレジストリへ登録したものの所有権。
    //   LoadedModel が破棄されると AssetRef が全部落ち、
    //   他に参照が無いものだけが解放される。
    //
    //   テクスチャをここに持たなくてよい理由: マテリアルが AssetRef で
    //   持っている（③-3）ので、マテリアルが解放されれば連鎖する。
    std::vector<scene::MeshRef> owned_meshes;
    std::vector<scene::MaterialRef> owned_materials;
};
```

- ★ **`spawn_model` が作ったエンティティは別途消す必要がある。** `LoadedModel` を破棄してもエンティティは残り、生ハンドルを握ったままになる。世代照合で `contains()` が `false` になるので既定メッシュ／既定マテリアルにフォールバックし、**落ちはしないが消えもしない**。
  - エンティティの一括破棄は「エンティティの生成・破棄を行う System」（phase17 の課題）とセット。このフェーズでは `spawn_model` が返す `std::vector<ecs::Entity>` を呼び出し側が持ち、手で `registry.destroy()` する。
  - ★ **破棄の順序は「エンティティ → LoadedModel」**。逆にすると1フレームだけ市松模様が出る。

### ③-7. 検証

```
// alive_count() をログに出す小さなヘルパを main.cpp に置く:
//   spdlog::info("meshes={} textures={} materials={}",
//                renderer.meshes().alive_count(),
//                renderer.textures().alive_count(),
//                renderer.materials().alive_count());
```

- **基本**: モデルをロード → 数が増える → `LoadedModel` を破棄 → **元の数に戻る**。
  - ★ 「元の数」は 0 ではない（既定アセットが4件残る。③-5）。残らなければ既定の永続参照が抜けている。
- **共有**: 同じモデルを2回ロード → 1回破棄 → **テクスチャが1枚も解放されないこと**（`by_path_` のキャッシュヒットで `ref_count == 2` になっているため。③-4）。
- **連鎖**: マテリアルを解放したとき、そのマテリアルだけが使っていたテクスチャも解放されること。他のマテリアルと共有しているテクスチャは残ること。
- **ABA**: 解放したハンドルを握ったまま描画 → 既定メッシュ／既定マテリアルへフォールバックし、**落ちない**こと（phase14 D-5 の世代番号が効いていることの確認）。
- **スロット再利用**: 解放 → 別のモデルをロード → 同じ `index` が再利用され、`generation` が上がっていること。
- 終了時にバリデーションのリーク報告が無いこと。
- ★ ロード／破棄を 100 回繰り返しても GPU メモリが増え続けないこと（タスクマネージャか `GpuAllocator` の統計で確認）。**これが ③ をやった理由そのもの。**

---

## 実装手順（この順で、各ステップ完了ごとに動作確認・コミット）

1. **⓪ オフスクリーン描画の土台** — `RenderPassConfig` → `PipelineConfig` 拡張 → `SamplerConfig` → set=2 のレイアウトとプール増枠 → **絵が phase15 と完全に同じことを確認**
2. **①-1 `draw_frame` の分解** — 収集をレンダーパスの外へ出し、private メソッドへ分ける → **絵が変わらないことを確認**（ここもリファクタのみ。①-2 以降と分けてコミットすること）
3. **①-2 〜 ①-7 シャドウマップ** — `ShadowMap` → `cast_shadows` と光源空間行列 → `CameraUBO` 拡張 → シャドウキャスタ収集 → シャドウパス記録 → `shadow.vert` → frag の `sample_shadow`
4. **①-8 〜 ①-10 調整と検証** — bias を 0 から上げていく → PCF → 床のある検証シーンで光源を回す
5. **②-9 スカイボックス**（★ **②の中で最初にやる**） — `Cubemap` → HDR 読み込み → `ComputePipeline` → equirect → cube → フルスクリーン三角形で背景に出す → **面の順序と上下が正しいことを目で確認**
6. **②-5 〜 ②-8 IBL 本体** — irradiance → prefiltered（mip ごとディスパッチ）→ BRDF LUT → frag の合成（`kAmbient` を削除）→ **マテリアルボールで metallic=1 が黒くないことを確認**
7. **③-1 〜 ③-3 `AssetRegistry<T>` の括り出し** — 参照カウント抜きで、既存の `unload()` をそのまま `destroy()` に載せ替えるところまで → **絵が変わらないことを確認**
8. **③-4 〜 ③-7 参照カウント** — `AssetRef` → 既定アセットの永続参照 → `LoadedModel` の所有 → ロード／破棄の繰り返しでメモリが増えないことを確認

> **① の難所は「配管」、② の難所は「式」、③ の難所は「寿命」**。性質が違うので、混ぜてコミットしないこと。
>
> 特に **2（`draw_frame` の分解）と 7（`AssetRegistry` の括り出し）は、絵が変わらないリファクタ**。ここを新機能と同じコミットに入れると、絵が壊れたときの二分探索が効かなくなる。
>
> ★ ⓪ と ①-1 を終えた時点で一度コミットしておくと、以降「壊れたのは土台か、影の式か」を後から切り分けられる（phase15 で ⓪ と ① の間にコミットを置いたのと同じ理由）。

---

## 検証観点（フェーズ全体）

- phase13/14/15 の性能特性が**維持**されていること（不透明がメッシュ種類数のドローに畳まれる、フラスタムカリングが効く、半透明の順序が保たれる、法線マップが正しく効く）。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で、**影と IBL を含めて**描画が継続すること。
  - ★ 特に `shadow_maps_` と IBL のキューブは `recreate_swapchain` で作り直さない。作り直していないことを、リサイズ後に影が出続けることで確認する。
- 終了時にバリデーションのリーク報告が無いこと。
- ライト数 0 / 1 / 64（上限）/ 65（超過クランプ）で破綻しないこと。
  - ★ **影を落とすライトが 0 個**の場合を必ず試すこと（`light_count.y == ~0u` の経路）。
- 影を落とすライトを複数立てた場合に、警告が出たうえで最初の1つだけが有効になること（D-4）。
- glTF モデルを大量（数十体）に配置しても破綻しないこと。**シャドウキャスタが加わってインスタンス数が増える**ので、`kMaxInstances = 4096` の超過クランプを3区間で再確認すること（D-5）。
- 起動時間が極端に伸びていないこと（IBL の前計算は数百 ms 以内。1秒を超えるならサンプル数か解像度が過大）。
- コンピュート非対応（あり得ないが）や HDR ファイル欠損で、**落ちずにフォールバック**すること。
  - ★ 環境マップが無い場合は「一様な灰色のキューブ」を生成して IBL を成立させること。落とすより、絵が地味になる方がよい。

---

## Phase 17 で行うこと（このフェーズではやらない・確定事項）

1. **カスケードシャドウマップ（CSM）** — ①-3 で「原点固定の正射影 1 枚」にした部分の解消。視錐台を距離で 3〜4 分割し、近いカスケードほどテクセル密度を上げる。**テクセル単位のスナップ**（カメラ移動時のシマリング対策）もここでセットにする。
2. **点光源のキューブシャドウ** — `samplerCubeShadow` と 6 面の描画。**プッシュ定数の素直な初使用**（面ごとの view-projection を渡す。D-3）。
3. **アルファマスクの影** — D-6 で割り切った部分。シャドウパス用のフラグメントシェーダで `discard`。set=1 をシャドウパスにもバインドし、`material_index` を頂点から渡す必要がある。
4. **エンティティの生成・破棄を行う System** — ③-6 で「呼び出し側が手で `registry.destroy()`」にした部分。`Registry&` を非 const で受ける形への拡張。`unload_model(registry, entities)` はこの上に乗る。

## さらに先の将来課題（plan18 以降）

- **法線行列の事前計算** — `InstanceData` に積む（phase15 ①-6 で頂点ごとの `inverse()` を許容した部分）。80 → 128 バイトになるので、帯域と計算量のトレードオフを測ってから決める。
- **ポストプロセスパス** — トーンマップをフラグメントシェーダの末尾から独立したパスへ出す。HDR のオフスクリーンカラーターゲット（`R16G16B16A16_SFLOAT`）が要る。ブルーム・FXAA・露出適応はその上。
  - ★ 出したら **⓪ の D-1（スワップチェーンを SRGB にする）の判断を見直すこと**。最終出力だけがエンコードする構造のまま、エンコード位置がポストプロセスの末尾へ移る。
- **ソフトシャドウの改善** — PCSS（可変半径 PCF）、VSM / ESM。①-8 の固定 3x3 PCF の次。
- **スポットライト**と、ライトのカリング（Forward+ / Clustered）。現状は全ピクセル × 全ライトの総当たり。
- **専用トランスファーキュー／専用コンピュートキュー**（キューファミリ跨ぎの所有権移譲）。②-3 でグラフィックスキューに相乗りした部分。
- **`GpuAllocator` → VMA 差し替え**（phase11 ③ で確保点を1箇所に閉じてある）。
- **OIT（順序独立透過）** — CPU ソートに依らない半透明。
- **プリミティブの拡充**（カプセル・円柱など。球は `add_sphere_mesh` として実装済み）。
- **キーコンフィグの設定ファイル入出力（JSON等）**（phase10 からの継続課題。nlohmann-json は tinygltf 経由で既に入っている）。
- **アニメーション / スキニング** — phase14 ③ で対応外にした部分。④ の階層が土台になる。
