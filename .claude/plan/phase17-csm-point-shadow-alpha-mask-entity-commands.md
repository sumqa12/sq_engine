# C++ ゲームエンジン（学習目的）— Phase 17: カスケードシャドウ + 点光源のキューブシャドウ + アルファマスクの影 + エンティティの生成・破棄

## Context

Phase 16 で**影と環境光と資産の寿命**が一通り揃った（[phase16-shadow-ibl-asset-refcount.md](phase16-shadow-ibl-asset-refcount.md) 参照）。方向光1枚の PCF 付きシャドウマップ、コンピュートで前計算した IBL、`AssetRegistry<T>` と `AssetRef` による参照カウントまでが動いている。

phase16 が**意図的に残した穴**は4つで、どれも plan に「phase17 で行う」と明記してある。

| 穴 | どこで妥協したか | 今の症状 |
|---|---|---|
| **影の解像度が距離に無関係** | phase16 ①-3「原点固定 + ±40 の正射影1枚」 | 手前の影はギザギザ、原点から 40 以上離れると影が出ない。カメラ追従にするとシマリングが出るので追従もしていない |
| **点光源が影を落とさない** | phase16 D-4「影を落とすのは方向光1つだけ」 | 点光源 20 灯がすべて壁を素通りして照らす |
| **アルファマスクの影が四角い** | phase16 D-6「シャドウパスはフラグメントシェーダを持たない」 | 葉や柵の板が四角い影を落とす。★ **調べたところ本パスでも `discard` していない**（[triangle.frag](../../shaders/triangle.frag) は `alpha_cutoff` を宣言しているだけ）。glTF の MASK は値が運ばれるだけで、絵には一度も反映されていない |
| **エンティティを安全に消す手段が無い** | phase16 ③-6「呼び出し側が手で `registry.destroy()`」 | `System::update` は `const Registry&` しか受け取れない。`each()` の中で `destroy` すると反復が壊れる。親を消すと子が `Parent` に死んだハンドルを握ったまま原点へ飛ぶ |

本フェーズで**初めて触る Vulkan / GLSL の要素**は次のとおり。

| 初めて触るもの | どこで要るか |
|---|---|
| **配列イメージの層ごとの描画**（層ごとの `VkImageView` / `VkFramebuffer`） | ② CSM、③ 点光源 |
| **`sampler2DArrayShadow`** | ② |
| **グラフィックスパイプラインのプッシュ定数** | ②③（D-3 で予告していた初使用） |
| **キューブ配列**（`VK_IMAGE_VIEW_TYPE_CUBE_ARRAY` / `samplerCubeArrayShadow` / `imageCubeArray` 機能） | ③ |
| **`gl_FragDepth` の書き込み** | ③ |
| **`discard`** | ① |
| **GLSL の `#include`** | ⓪ |

例によって Claude は宣言・骨格・TODO コメントまでを書き、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-0. 着手順は ⓪ → ① → ② → ③ → ④

| 順 | 項目 | 理由 |
|---|---|---|
| 0 | **⓪ 複数シャドウの土台** | シェーダの共通定義・描画リスト・プッシュ定数・シャドウマップの層・`LightData` を、影が増えても破綻しない形へ先に広げる。**絵は1ピクセルも変わらない** |
| 1 | **① アルファマスク** | 4項目の中で一番小さく、結果が目で一瞬で分かる。ここで「シャドウパスにフラグメントシェーダと set=1 が入る」ので、②③はその形をそのまま引き継げる |
| 2 | **② カスケードシャドウ（CSM）** | ⓪の「層ごとの描画」の最初の利用者。方向光1つのまま、影の質だけを上げる |
| 3 | **③ 点光源のキューブシャドウ** | ②と同じ配管（層・プッシュ定数・区間）に乗る。違うのは行列の作り方と深度の中身だけ |
| 4 | **④ エンティティの生成・破棄** | ①〜③と**完全に独立**。絵が変わらない ECS 側の作業なので最後に置く（phase16 ③と同じ理由） |

**① を ②③ より先にする理由**: ユーザーが挙げた順は「CSM → 点光源 → アルファマスク」だが、アルファマスクを最後にすると、②③で作ったシャドウパス（CSM 用・点光源用の2系統）の**両方**へ後から `discard` を入れて回ることになる。先にシャドウパスへフラグメントシェーダを入れておけば、②③は最初からそれを前提に書ける。

### D-1. 描画リストを「区間の列」に一般化する

現在の `instances_` は「不透明 | 半透明 | シャドウキャスタ」の3区間で、区間ごとに `opaque_items_` などのメンバと `first_instance` の計算が手書きされている。phase17 では区間が**最大 30 本**になる。

```
instances_ = [ opaque | transparent | cascade 0 | cascade 1 | cascade 2 | cascade 3 | point 0 face 0 | ... | point 3 face 5 ]
               ↑ 本パス              ↑ ② CSM（4本）                                 ↑ ③ 点光源（4灯 × 6面 = 24本）
```

- 区間を `DrawList { std::vector<DrawItem> items; std::uint32_t first_instance; }` の**並び**として持ち、`upload_instances` は並びの順に詰めて `first_instance` を埋める。
- ★ **`kMaxInstances` 超過時のクランプは「並びの末尾から捨てる」だけになる。** 並びを「重要な順」にしておけば（本パス → 近いカスケード → 遠いカスケード → 点光源）、超過したときに消えるのは遠い影から。phase16 ①-5 で手書きした3段の分岐が一般化される。
- ★ `kMaxInstances` は **4096 → 16384** に上げる。カスケードの境界をまたぐ物体は複数の区間に重複して入るため、1000体の格子だけで影の区間が数千に達する。80 バイト × 16384 = 1.25 MiB × `kFramesInFlight`。

### D-2. 1枚のシャドウマップを「層を持つイメージ」に一般化する

| 用途 | 層の数 | サンプル用ビュー | 描画用ビュー |
|---|---|---|---|
| phase16 の方向光 | 1 | `2D` | 層ごとの `2D`（1本） |
| ② CSM | 4 | `2D_ARRAY` | 層ごとの `2D`（4本） |
| ③ 点光源 | 6 × 4 = 24 | `CUBE_ARRAY` | 層ごとの `2D`（24本） |

- 3つとも「**サンプルするときは全層をまとめて1本のビューで読み、描くときは1層ずつ別のフレームバッファへ描く**」という同じ形をしている。違うのは層数とサンプル用ビューの型だけなので、`ShadowMap` クラス1つを設定構造体で作り分ける。
- **採らない案: CSM を4枚の独立した `ShadowMap` にする**。シェーダ側が `sampler2DShadow` × 4 の配列になり、カスケード番号で添字を引く（動的インデックス）ことになる。`sampler2DArrayShadow` なら層番号は**テクスチャ座標の1成分**にすぎない。
- **採らない案: ジオメトリシェーダ／multiview で全層を1パスで描く**。パス数が減る代わりに、新しい機能が2つ増える。まず「層ごとに Begin/End するのが正しく動く」ことを確かめてから、将来課題として検討する。

### D-3. プッシュ定数は**全グラフィックスパイプラインに同じレンジ**を宣言する

phase16 D-3 で「プッシュ定数レンジが違うパイプライン同士はレイアウト互換でなくなり、パイプラインを切り替えた瞬間にバインド済みのディスクリプタセットが外れる」と書いた。そのため、プッシュ定数を**使うのはシャドウパスだけ**でも、レンジは**本パス・半透明・スカイボックスを含む全パイプライン**に同じものを宣言する。

```cpp
// 新規 shadow_push_constants.hpp
struct ShadowPushConstants {
    glm::mat4  view_projection;  // offset  0, 64  ③ 点光源の「この面」の行列（② では未使用）
    glm::uvec4 indices;          // offset 64, 16  x = カスケード番号（②）
                                 //                y = ライトの添字（③。frag が lights[] から位置と far を引く）
                                 //                z, w = 未使用
};
static_assert(sizeof(ShadowPushConstants) == 80);   // 保証下限 128 バイトに収まる
// レンジ: stageFlags = VERTEX | FRAGMENT, offset = 0, size = 80
```

**② と ③ で渡し方が違う理由**（phase16 D-3「同じ値を2経路で運ばない」の適用）:

- ② CSM: カスケードの行列は**本パスの frag も読む**（どの段で影を引くか）。だから行列は `CameraUBO` に置き、シャドウパスへは**番号だけ**を渡す。行列そのものを push すると、UBO とプッシュ定数の2経路になる。
- ③ 点光源: 面ごとの行列は**シャドウパスしか読まない**（本パスはキューブを方向ベクトルで引くので行列が要らない）。だから行列そのものを push してよい。一方、ライトの位置と far は本パスも読むので、こちらは**添字だけ**を渡して `lights[]` から引く。

- ★ 本パスのシェーダは `push_constant` ブロックを**宣言しない**。レイアウトにレンジがあってもシェーダが読まなければ、`vkCmdPushConstants` を一度も呼ばなくて問題ない。

### D-4. 影の情報は `LightData` の4本目の `vec4` に持たせる（`light_count.y` を廃止）

phase16 D-4 は「影を落とすライトは1つ」なので `CameraUBO::light_count.y` に添字を1つ入れて済ませた。phase17 では方向光1 + 点光源4 の**最大5灯**が影を落とすので、`uvec4` の空き成分には入りきらない。

```cpp
struct LightData {
    glm::vec4 position_type;    // offset  0  （変更なし）
    glm::vec4 direction_range;  // offset 16  （変更なし）
    glm::vec4 color_intensity;  // offset 32  （変更なし）
    glm::vec4 shadow;           // offset 48  ← phase17 ⓪-2 で追加
                                //   x = シャドウスロット（-1 = 影なし）。
                                //       Directional: 0（CSM。方向光の影は1つだけ）
                                //       Point:       0..kMaxPointShadows-1（キューブ配列の何番目か）
                                //   y = near（Point のみ）
                                //   z = far （Point のみ。= range）
                                //   w = 未使用
};
static_assert(sizeof(LightData) == 64);
```

- phase15 D-4 で「種別を `position_type.w` に float で入れる」と決めたのと同じ流儀。スロット番号も float で入れ、シェーダ側で `int(light.shadow.x)` として読む。
- `light_count.y` は**デバッグフラグ**（② のカスケード色分け）に転用する。
  - ★ **転用するときは `triangle.frag` の `if (i == camera.light_count.y)` を必ず消すこと。** 残すと「デバッグフラグの値と同じ番号のライト」に影が付く。フラグが 0 なら**0番のライト**に影が付き、それがたまたま太陽なので**絵が正しく見えてしまう**。

### D-5. シャドウパスのカリングを `NONE` にし、アクネは bias で取る

phase16 ①-8 は前面カリング（`VK_CULL_MODE_FRONT_BIT`）でアクネを減らした。phase17 ではこれを `NONE` に戻す。理由は2つで、どちらも phase17 の作業から直接出てくる。

1. **アルファマスクの板は片面のことが多い**（①）。前面カリングだと「裏をライトに向けた板だけが影を落とす」。phase16 の検証シーンの注記（[main.cpp](../../sandbox_graphics/main.cpp) の床のブロック）に書いた「板の影が出たり出なかったりする」が、葉の板で必ず起きる。
2. **CSM はカスケードごとにテクセルの大きさが違う**（②）。bias をテクセルの大きさに比例させる仕組み（D-8）を入れる以上、カリングでごまかす意味が薄れる。

- ★ 代償として、**床のような「ライトの方を向いた面」が自分自身の深度を書く**ようになる。bias が 0 なら必ず縞が出る。これは phase16 ①-8 の手順1「縞が出るのが正常」が、カリングを変えずに確認できるようになったということでもある。
- ★ `NONE` なら三角形の巻き順は関係なくなる。③ の点光源の行列は Y 反転をしない（③-3）ので巻き順が本パスと逆になるが、`NONE` のおかげで `frontFace` を気にしなくてよい。**将来カリングを戻すときは、この点を思い出すこと。**

### D-6. アルファマスクは「全キャスタ共通のシャドウ用 frag」で `discard` する

- 採る案: シャドウパスのフラグメントシェーダを1本にし、`alpha_cutoff > 0` のマテリアルだけ `discard` する。
- **採らない案: MASK 用とそれ以外でシャドウパイプラインを分ける**。MASK でないキャスタは frag を持たない方が速い（`discard` を含むシェーダは早期深度テストが効きにくい）が、区間が2倍（30 → 60本）になり、収集フェーズでマテリアルの中身まで覗く必要が出る。
  - 今のシーン規模では差が測れないはず。**測ってから**分ければよい（将来課題）。
- ★ シャドウパスで set=1（bindless テクスチャ + マテリアル SSBO）をバインドする。phase16 ①-6 の「set=1 はシャドウパスでバインドしない」は撤回。
  - set=2 は**引き続きバインドしない**（set=2 binding=0 はシャドウパスが書いている当のイメージ。phase16 ①-1）。

### D-7. CSM は4段、影の最大距離を設け、各段を境界球で包んでテクセル単位でスナップする

| 項目 | 決定 | 理由 |
|---|---|---|
| 段数 | **4**（`kCascadeCount`） | 定番。`vec4` 1本に分割距離が収まる |
| 影の最大距離 | **100**（`kShadowMaxDistance`） | カメラの far は 500。500 まで4段で割ると1段あたりが広すぎ、近くの段まで粗くなる。100 より先は影なし |
| 分割 | **practical split**（λ = 0.75） | 対数分割と一様分割の混合。λ=1 の純対数は近い段が狭すぎ、λ=0 の一様は近い段が広すぎる |
| 段の包み方 | **境界球** | AABB で包むと、カメラが回るたびに箱の大きさが変わり、テクセルの大きさも変わって影の縁が揺れる。球は回転しても大きさが変わらない |
| シマリング対策 | **テクセル単位のスナップ** | 正射影の原点をシャドウマップの1テクセルの整数倍にそろえる。カメラが平行移動しても、テクセルの格子がワールドに対して動かない |
| 解像度 | **2048**（1層あたり） | 4層 × 2 フレーム = 8層 × 16 MiB = 128 MiB（D32 の場合）。重いと感じたら 1024 に下げる（②-5） |

- **採らない案: 深度クランプ（`depthClampEnable`）でライト側の手前の物体を拾う**。正射影の near より手前にあるキャスタを near に押し付けて描く方法で、`depthClamp` 機能の有効化とパイプライン設定が要る。phase17 では「正射影の near を光源側へ `kCascadeCasterMargin` だけ伸ばす」で代える（②-3）。新しい機能を増やさず、効果は同じ。
- ★ 境界球は大きめ（保守的）なので、テクセル密度は AABB より下がる。**揺れないことの方が価値が高い**というのがこの判断。

### D-8. 法線オフセットはワールド空間で、段のテクセル幅に比例させる

phase16 の `kNormalBiasMax / kNormalBiasMin` は「NDC の z 空間での値」だった。正射影の z 範囲が1つだったから成立していた。

- CSM では**段ごとに z 範囲もテクセルの大きさも違う**ので、1つの定数では全段に合わない（近い段に合わせると遠い段で縞、遠い段に合わせると近い段で peter-panning）。
- 代わりに、**比較する前にワールド座標を法線方向へずらす**（normal offset）。ずらす量は「その段の1テクセルのワールド幅 × 係数 × sinθ」。テクセル幅は `CameraUBO::cascade_texel_sizes` で渡す。
- **角度の係数は sinθ**（θ = 法線 N と光の向き L のなす角。`sqrt(1 - NdotL²)`）。
  - 縞が出るのは、PCF が読む範囲（±1〜2 テクセル）の中で、受光面の深度が光の方向へ傾いているから。その傾きぶんを越えるのに要るずらし量は sinθ に比例する（光に正対する面 θ = 0 では不要、かすめる面 θ → 90° で最大）。
  - ★ **当初は `(1 - NdotL)`（= 1 − cosθ）にしていたが、誤りだった**（②-10 の調整で判明）。θ が小さい（太陽が高い）ほど sinθ に対して極端に小さくなり（NdotL = 0.95 で約 1/6）、その不足を `kNormalOffsetScale` を大きくして補うことになる。太陽の高さ約 35° のシーンで 7.0 まで上げないと縞が消えず、しかもその値は太陽をさらに高くすると足りなくなる。
- `vkCmdSetDepthBias` は**段ごとに**呼べる（動的ステート）。slope は共通、constant は段ごとに変えてもよい。
  - ★ ②-10 の調整では、slope / constant とも 0 で影は浮かず、縞も法線オフセットだけで消えた（slope は各段のラスタライズの中で掛かるので、段ごとのスケールにもともと追従している）。仕組みは残し、値は 0 とする。

### D-9. 点光源の影は「線形距離」を深度に書き、`samplerCubeArrayShadow` で比較する

点光源の影は、キューブの6面を90°の透視投影で描く。問題は**何を深度として書くか**。

- (a) 透視投影の深度（非線形）をそのまま書く … 比較側で「方向ベクトルの主軸成分から、その面の射影深度を逆算する」式が要る。bias が距離によって効き方を変える
- (b) **`gl_FragDepth = length(frag_world - light_pos) / far` を書く** … 採用

**(b) を採る**。

1. 比較側の式が `length(v) / far` だけになる。方向ベクトル `v` はキューブのルックアップにもそのまま使える。
2. bias を**ワールド単位**で考えられる（「5cm 手前」が `0.05 / far`）。
3. どのみち ① でシャドウパスに frag が入るので、frag を持つこと自体のコストは増えない。

- ★ **代償**: `gl_FragDepth` を書くと、ラスタライザが付ける**depth bias（`vkCmdSetDepthBias`）は無効になる**（bias はラスタライズした深度に足されるもので、frag が上書きすれば消える）。点光源の bias は**比較側（本パスの frag）だけ**で取る。点光源用パイプラインは `depth_bias_enable = false` にすること。
- ★ 影を落とせる点光源は**最大4灯**（`kMaxPointShadows`）。キューブ配列は 6 × 4 = 24 層。`cast_shadows == true` の点光源のうち**最初の4つ**を選び、5つ目以降は警告して無視する（phase16 D-4 と同じ流儀）。
  - ★ 「カメラに近い順に4つ」の方が実用的だが、フレームごとにスロットの割り当てが変わり、デバッグしにくい。まず固定で通す（将来課題）。
- ★ `CUBE_ARRAY` のビューは `imageCubeArray` 機能が要る。デバイス作成時に有効化すること（③-2）。忘れるとビュー作成がバリデーションエラーになる（気付ける壊れ方）。

### D-10. エンティティの構造変更は「コマンドバッファ」に積み、同期点でまとめて流す

④ の本質は「**反復中に構造を変えてはいけない**」と「**`System` は `const Registry&` しか持たない**」の2つをどう両立させるか。

- (a) `System::update` を `Registry&`（非 const）にする … `each()` の中で `destroy` されたら壊れる、という罠はそのまま残る
- (b) 破棄予定のタグコンポーネント（`PendingDestroy`）を `add` する … `add` 自体がアーキタイプ移動なので、`each()` の中では同じく壊れる
- (c) **`EntityCommands`（コマンドバッファ）に積み、全 System の後で `flush(Registry&)` する** … 採用

**(c) を採る**。Unity DOTS の `EntityCommandBuffer`、Bevy の `Commands` と同じ考え方。

- System は**読むのは `const Registry&`、変えるのは `EntityCommands&`** という形になる。構造変更が「いつ起きるか」が `SystemScheduler::update` の末尾の1点に固定される。
- 積めるのは2種類だけにする:

| コマンド | 意味 |
|---|---|
| `destroy(e)` | そのエンティティだけを消す |
| `defer(fn)` | `std::function<void(Registry&)>` を同期点で実行する（生成・コンポーネントの付け外しはこれで書く） |

- **子孫ごと消す**のは `ecs` ではなく `scene` 側のフリー関数 `scene::destroy_hierarchy(registry, commands, e)` が担う。`Parent` は `scene` の概念で、`ecs` は階層を知らない層のまま保つ（`ecs` → `scene` の依存を作らない）。この関数は `const Registry&` を読んで子孫を列挙し、1体ずつ `commands.destroy()` を積むだけ。

- **採らない案: `create()` をコマンドにして、仮の `Entity` を即座に返す**（Bevy の `spawn` 相当）。仮 ID と本 ID の対応表が要り、ECS 本体に手を入れる範囲が大きい。`defer` の中で `registry.create()` すれば同じことができる。
- ★ `flush` は**死んだエンティティを黙って飛ばす**こと。`destroy_hierarchy(親)` と `destroy(子)` が同じフレームに積まれると、子は2回消されそうになる。`Registry::destroy` は死んだハンドルで例外を投げる（`record_of`）。

---

## ⓪ 複数シャドウの土台

**このステップの完了条件は「phase16 と完全に同じ絵が出ること」。**

### ⓪-1. シェーダの共通定義を `#include` に括り出す（新規 `shaders/include/*.glsl`）

`CameraUBO` のブロック定義は現在 **4本**（`triangle.vert` / `triangle.frag` / `shadow.vert` / `skybox.vert`）に重複している。phase17 では `CameraUBO` が大きく変わり（②-4）、それを読むシェーダも **7本**に増える。phase16 ①-4 の「3箇所で揃える」は、もう手作業では守れない。

```
shaders/include/
├── camera.glsl      // CameraUBO（set=0 binding=0）
├── instance.glsl    // InstanceData + InstanceBuffer（set=0 binding=1）
├── light.glsl       // LightData + LightBuffer（set=0 binding=2）
├── material.glsl    // MaterialData + MaterialBuffer（set=1 binding=1）+ textures[]（set=1 binding=0）
└── shadow_push.glsl // ShadowPushConstants（D-3。②以降で使う）
```

```glsl
// camera.glsl の形（他も同じ）
#ifndef SQ_CAMERA_GLSL
#define SQ_CAMERA_GLSL
// ★ C++ 側 scene::CameraUBO と対。メンバを変えたら static_assert も直すこと。
layout(set = 0, binding = 0) uniform CameraUBO {
    ...
} camera;
#endif
```

```glsl
// 使う側
#version 450
#extension GL_EXT_nonuniform_qualifier : require   // ★ material.glsl を include するなら、include より**前**に書く
#include "camera.glsl"
#include "material.glsl"
```

```cmake
# sandbox_graphics/CMakeLists.txt の変更
file(GLOB SHADER_INCLUDES ${SHADER_SOURCE_DIR}/include/*.glsl)

# add_custom_command に2点足す:
#   COMMAND ${glslc_executable} -I ${SHADER_SOURCE_DIR}/include -o ... ${source}
#   DEPENDS ${source} ${SHADER_INCLUDES}
#
# ★ DEPENDS に include を足さないと、camera.glsl を直しても**どのシェーダも再コンパイルされない**。
#   古い .spv のまま動き、C++ 側だけが新しいレイアウトになる（= phase16 ①-4 の「鏡面反射だけおかしい」型の壊れ方）。
#   「include を1つ変えたら全シェーダを作り直す」のは過剰だが、シェーダは十数本なので問題にならない。
#   正確な依存（glslc -MD + DEPFILE）は将来課題。
# ★ file(GLOB) は *.vert / *.frag / *.comp しか拾わないので、include/*.glsl が
#   単独でコンパイルされることはない（拡張子を .glsl にする理由）。
# ★ GLOB を足したので CMake の再実行が要る。
```

- ★ glslc は `#include` を標準で扱える（`GL_GOOGLE_include_directive` の宣言は不要）。
- ★ include ガードは `#pragma once` ではなく `#ifndef` で書く（glslang の対応状況に依存しない）。

### ⓪-2. `LightData` に `shadow` を足す（D-4）

```
// light_data.hpp: vec4 shadow を足し、static_assert を 48 → 64 へ。offsetof(shadow) == 48 も足す。
// light.glsl:    struct LightData に vec4 shadow を足す（⓪-1 で1箇所になっている）。
//
// collect_lights:
//   シャドウキャスタに選んだ方向光の LightData::shadow.x = 0.0f、それ以外は -1.0f。
//   ★ shadow の既定値を {-1, 0, 0, 0} にしておくこと。{0, 0, 0, 0} のままだと
//     全ライトが「スロット 0 の影を持つ」扱いになり、**全ライトに太陽の影が掛かる**。
//   ★ FrameContext::shadow_light_index は残す（② のカスケード計算で「方向光が居るか」の判定と、
//     光の向きの取り出しに使う）。
//
// CameraUBO::light_count.y: シャドウライトの添字 → デバッグフラグ（この段階では 0 を入れる）。
//
// triangle.frag:
//   if (i == camera.light_count.y) → if (int(light.shadow.x) >= 0 && type == 0)
//   ★ D-4 の注記: 古い比較を残すと、フラグ 0 = 0番のライト（= 太陽）に影が付いて**正しく見えてしまう**。
//     太陽を作る順番を変える（点光源の後に作る）と気付ける。
```

### ⓪-3. 全パイプラインに共通のプッシュ定数レンジ（D-3）

```cpp
// graphics_pipeline.hpp: コンストラクタに引数を1本足す。
GraphicsPipeline(VkDevice device, VkRenderPass render_pass, VkExtent2D viewport_extent,
                 const std::string& vert_spv_path, const std::string& frag_spv_path,
                 const std::vector<VkDescriptorSetLayout>& set_layouts,
                 const VkPushConstantRange& push_constant_range,   // ← phase17 ⓪-3
                 const PipelineConfig& config);
//   ★ 既定引数にしないこと（frag_spv_path と同じ理由。D-3 の「全パイプラインで同じ」を
//     呼び出し側に毎回書かせることで、1本だけ違うレンジを渡す事故を防ぐ）。
//   ★ size == 0 なら pushConstantRangeCount = 0（ComputePipeline と同じ規則）。
```

```
// renderer.hpp:
//   static constexpr VkPushConstantRange kPushConstantRange{
//       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPushConstants) };
//   set_layouts と同じく、**全パイプラインにこの1つを渡す**。
//
// ★ vkCmdPushConstants の stageFlags は、レンジの stageFlags と**一致**させること
//   （VERTEX だけで push すると「レンジと一致しない」でバリデーションエラー）。
```

### ⓪-4. 描画リストの一般化（D-1）

```cpp
// renderer.hpp
struct DrawList {
    std::vector<DrawItem> items;
    std::uint32_t first_instance = 0;   // upload_instances が埋める
};

DrawList opaque_;
DrawList transparent_;
std::array<DrawList, kCascadeCount> cascade_lists_;                     // ② で使う（⓪ では [0] だけ）
std::array<DrawList, kMaxPointShadows * 6> point_shadow_lists_;         // ③ で使う（⓪ では空）

// 並び（= instances_ に詰める順 = クランプで残る優先順位）を1か所で返す。
//   ★ この関数の並びを変えると first_instance が全部ずれる。並びはここ以外で書かないこと。
[[nodiscard]] std::array<DrawList*, 2 + kCascadeCount + kMaxPointShadows * 6> draw_lists_in_order();
```

```
// upload_instances():
//   1. 並びの順に items を instances_ へ詰め、各 DrawList::first_instance を埋める
//   2. instances_.size() > kMaxInstances なら、**並びの末尾の DrawList から** items を削る
//      ★ instances_ を切り詰めるだけでは駄目。DrawList::items も同じだけ削らないと、
//        転送していない範囲を描く（phase16 ①-5 と同じ注記）。
//
// record_draw_items の引数を (items, first_instance) → (const DrawList&) にする。
//
// ⓪ の段階では shadow_items_ を cascade_lists_[0] に置き換えるだけ。絵は変わらない。
```

### ⓪-5. `ShadowMap` を層対応にする（D-2）

```cpp
struct ShadowMapConfig {
    std::uint32_t size = 2048;                                // 1層の一辺
    std::uint32_t layers = 1;                                 // CSM = 4、点光源 = 6 × kMaxPointShadows
    VkImageViewType sample_view_type = VK_IMAGE_VIEW_TYPE_2D; // 2D / 2D_ARRAY / CUBE_ARRAY
    //   ★ CUBE / CUBE_ARRAY のときは VkImageCreateInfo::flags に CUBE_COMPATIBLE_BIT が要る
    //     （phase16 D-8 のキューブマップと同じ。sample_view_type から自動で決めること）。
};

class ShadowMap {
public:
    ShadowMap(VkDevice device, GpuAllocator& allocator,
              std::uint32_t queue_family_index, VkQueue queue,   // ← 初期遷移用（下記）
              VkRenderPass shadow_render_pass, VkFormat depth_format,
              const ShadowMapConfig& config);

    [[nodiscard]] VkImageView sample_view() const;                   // set=2 に書く（全層）
    [[nodiscard]] VkFramebuffer framebuffer(std::uint32_t layer) const; // 層ごと
    [[nodiscard]] VkExtent2D extent() const;
    [[nodiscard]] std::uint32_t layers() const;

private:
    // ...
    VkImageView sample_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> layer_views_;      // 層ごとの 2D ビュー（baseArrayLayer = i, layerCount = 1）
    std::vector<VkFramebuffer> framebuffers_;   // 層ごと
};
```

- ★ **コンストラクタで全層を `DEPTH_STENCIL_READ_ONLY_OPTIMAL` へ遷移しておく**（`SingleTimeCommands` + `image_utils` の遷移関数。深度の aspect と `UNDEFINED → DEPTH_STENCIL_READ_ONLY_OPTIMAL` の枝を足す）。
  - 理由: サンプル用ビューは**全層**を指す。一度も描かれていない層が `UNDEFINED` のまま残ると、本パスがそのビューを読んだ瞬間に「レイアウト不一致」のバリデーションが出る。③ では使われていないスロットの6層が毎フレーム描かれないので、必ず踏む。
  - phase16 ①-6 は「描くものが無くても Begin/End だけは通す」で同じ問題を避けていた。初期遷移を入れれば**描くものが無い層はパスごと飛ばしてよい**ことになり、記録側が単純になる。
- ★ 層ごとのビューは `aspectMask = DEPTH` だけ（phase16 ①-2 の注記と同じ）。
- ⓪ では `ShadowMapConfig{ .size = 2048, .layers = 1, .sample_view_type = 2D }` で作り、phase16 と同じ1枚を再現する。

### ⓪-6. 検証

- **絵が phase16 と完全に同じであること**（ここが完了条件）。
- バリデーション層に新しい警告が1つも出ないこと。特にプッシュ定数レンジ（⓪-3）とレイアウト遷移（⓪-5）。
- ★ ⓪-1 の確認: `camera.glsl` にわざと1行コメントを足してビルドし、**全シェーダが再コンパイルされる**ことをログで見る。
- ★ ⓪-2 の確認: 太陽の生成を点光源の**後**へ移しても、影が太陽にだけ付くこと（D-4 の罠の確認）。

---

## ① アルファマスクの影（と本パスの `discard`）

### ①-1. 本パスの `discard`（[triangle.frag](../../shaders/triangle.frag)）

```glsl
// main() のテクスチャを読んだ直後（ライティングの計算より前）に置く:
float alpha = albedo_sample.a * m.base_color.a;
if (m.alpha_cutoff > 0.0 && alpha < m.alpha_cutoff) { discard; }
//   ★ alpha_cutoff == 0 は「MASK 無効」（phase15 ②-2 の約束）。0 との比較を省くと、
//     alpha が 0 の半透明（BLEND）マテリアルの画素まで消える。BLEND は半透明パスで混ぜるもので、
//     消すものではない。
//   ★ ライティングの計算より**前**に置くこと。後ろに置いても結果は同じだが、捨てる画素に
//     ライト 65 灯ぶんの計算をしてから捨てることになる。
```

### ①-2. シャドウ用のフラグメントシェーダ（新規 `shaders/shadow.frag`）

phase16 は「`shadow.frag` を置かないこと」と書いたが、D-6 で方針が変わったので**置く**。

```glsl
// shadow.vert の変更: uv と material_index を出力する。
layout(location = 2) in vec2 in_uv;                  // ★ location は triangle.vert と同じ番号（2）
layout(location = 0) out vec2 frag_uv;
layout(location = 1) out flat uint frag_material_index;

// shadow.frag（新規）:
#extension GL_EXT_nonuniform_qualifier : require
#include "material.glsl"
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_material_index;
void main() {
    // ①-1 と同じ判定で discard するだけ。カラー出力（out）は宣言しない。
    //   ★ out を宣言すると「カラーアタッチメントが無いのに出力がある」になる（警告）。
}
```

- ★ パイプラインの frag を `""` から `"shaders/shadow.frag.spv"` に変える。`PipelineConfig::has_color_attachment = false` はそのまま（frag はあるが色は出さない）。
- ★ 新しい `.frag` を足したので CMake の再実行が要る。

### ①-3. シャドウパスで set=1 をバインドする（D-6）

```
// record_shadow_pass:
//   set=0 に続けて set=1（textures_->bindless_set()）もバインドする。
//   ★ set=2 はバインドしない（変わらず）。
//
// ★ ディスクリプタセットのステージに注意:
//   set=1 の binding=0（テクスチャ配列）と binding=1（マテリアル SSBO）はどちらも
//   stageFlags = FRAGMENT なので、shadow.frag から読めば足りる。追加の変更は要らない。
//   set=0 binding=1（インスタンス SSBO）は VERTEX のみで、material_index は vert で読んで
//   flat で渡すので、これも変更は要らない（triangle.vert と同じ形）。
```

### ①-4. カリングを `NONE` にし、bias を調整し直す（D-5）

```
// pipeline_shadow_ の PipelineConfig: cull_mode = FRONT → NONE
//
// ★ 変えた直後は床に縞（アクネ）が出るはず。出なければ配管を疑う（phase16 ①-8 の手順1）。
//   kDepthBiasSlope を上げて消す。phase16 の値（1.25 / 1.75）では足りない可能性が高い。
// ★ ② で bias の仕組み自体が段ごとに変わる（D-8）ので、ここでは「床の縞が消える」までで止めてよい。
```

### ①-5. 検証シーン（[main.cpp](../../sandbox_graphics/main.cpp)）

```
// MASK マテリアルを手で作る（glTF の MASK モデルはアセットに無いため）。
//
//   1. αチャンネルを持つ PNG を1枚選ぶ。hsr_icon_01 のアイコンは背景が透明のものがあるはず。
//      ★ 選んだ画像に本当に α があるか、画像ビューアで背景が市松模様に見えるかで確かめること。
//        α の無い画像だと alpha = 1.0 で何も消えず、「discard が効いていない」と誤解する。
//   2. MaterialData{ .base_color = {1,1,1,1}, .roughness = 0.8, .alpha_cutoff = 0.5 } に
//      { .albedo = その画像 } を付けて登録する。
//      ★ Material::transparent は false（MASK は不透明パスで描く）。
//   3. plane_mesh を縦に立てて（X 軸まわりに 90°）床の上に数枚並べる。
//      ★ 太陽に**表を向けた板**と**裏を向けた板**を両方置くこと。D-5 で NONE にしたので、
//        両方が影を落とすはず。FRONT のままだと片方しか落とさない。
```

### ①-6. 検証

- 板の**透明な部分が描かれない**こと（本パスの `discard`）。
- 床に落ちる影が**アイコンの形**に抜けること（シャドウパスの `discard`）。
- 表を向けた板と裏を向けた板の**両方**が影を落とすこと（D-5）。
- 半透明（`transparent = true`）の物体の見た目が変わらないこと（`alpha_cutoff == 0` の約束）。
- MASK でない物体の影が変わらないこと。

| 症状 | 最初に疑うところ |
|---|---|
| 板が四角いまま描かれる | 画像に α が無い ／ `alpha_cutoff` が 0 のまま ／ `discard` の比較の向き |
| 板は抜けるが影が四角い | シャドウパイプラインの frag が `""` のまま ／ set=1 をバインドしていない |
| 影が全部消える | `shadow.vert` の出力 location と `shadow.frag` の入力 location の食い違い（uv がゼロ → α がゼロ） |
| 半透明の物体の一部が消える | `alpha_cutoff > 0.0` の判定を省いている |
| 片方の向きの板だけ影が出る | シャドウのカリングが FRONT / BACK のまま |

---

## ② カスケードシャドウマップ（CSM）

### ②-1. カメラのパラメータを `FrameContext` に持たせる

```cpp
// カスケードを切るには「カメラの視錐台を距離で輪切りにする」必要があり、
// view_projection（合成済みの行列）だけでは足りない。
struct FrameContext {
    glm::mat4 view_projection{ 1.0f };
    glm::mat4 view{ 1.0f };             // ← phase17 ②-1
    glm::vec3 camera_position{ 0.0f };
    glm::vec3 camera_forward{ 0.0f, 0.0f, -1.0f };  // ← 正規化済み（frag のビュー深度計算に使う）
    float fov_y = 0.0f;                 // ← 輪切りの透視投影を作り直すため
    float aspect = 1.0f;
    float near_plane = 0.1f;
    float far_plane = 500.0f;
    // ...（既存のライト関係はそのまま）
};
```

- ★ `resolve_camera` の3段フォールバック（phase10 D-2）の**既定ビュー**でも、これらを埋めること。`scene::Camera::default_view_projection` は合成済みの行列しか返さないので、既定値を `scene::Camera{}` の初期値として書き直すのが素直（既定カメラも `Camera` 構造体1つで表す）。

### ②-2. 分割距離（practical split）

```
// n = context.near_plane
// f = min(context.far_plane, kShadowMaxDistance)
// for i in 1..kCascadeCount:
//     p       = i / kCascadeCount
//     log     = n * pow(f / n, p)
//     uniform = n + (f - n) * p
//     split[i - 1] = mix(uniform, log, kCascadeSplitLambda)   // λ = 0.75
//
// split[i] は「i 段目の**遠い側**のビュー深度」。i 段目の近い側は split[i - 1]（0 段目は n）。
// ★ ビュー深度は「カメラからの距離」ではなく「視線方向への射影」（dot(p - cam, forward)）。
//   距離で判定すると、画面の端で段の境界が円弧になる。
```

### ②-3. 段ごとの光源行列（D-7）

```
// compute_cascade(light_dir, context, split_near, split_far) -> { view_projection, texel_size }
//
// 1. 輪切りの8頂点をワールドで求める
//      proj_slice = perspective(fov_y, aspect, split_near, split_far)
//      inv = inverse(proj_slice * context.view)
//      NDC の8隅（x,y ∈ {-1,1}, z ∈ {0,1}）を inv で戻し、w で割る
//      ★ z は [0,1]（GLM_FORCE_DEPTH_ZERO_TO_ONE）。-1 を使うと near の外側の点が混ざる。
//
// 2. 境界球
//      center = 8頂点の平均
//      radius = center から8頂点への距離の最大
//      radius = ceil(radius * 16) / 16      ★ 半径の浮動小数の揺れでテクセル幅が毎フレーム変わるのを防ぐ
//
// 3. 光源の view / proj
//      eye  = center - light_dir * (radius + kCascadeCasterMargin)
//      view = lookAt(eye, center, up)       ★ up の縮退分岐は phase16 ①-3 と同じ
//      proj = ortho(-radius, radius, -radius, radius, 0, 2 * radius + kCascadeCasterMargin)
//      proj[1][1] *= -1                     ★ phase16 と同じく Y 反転（向きを揃えておく）
//      ★ near を光源側へ kCascadeCasterMargin だけ伸ばす理由（D-7）:
//        輪切りの外（光源側）にある物体も、輪切りの中へ影を落とす。伸ばさないと
//        「高い建物の影が、カメラを近づけた途端に消える」。
//
// 4. テクセル単位のスナップ（シマリング対策の本体）
//      shadow = proj * view
//      origin = shadow * vec4(0, 0, 0, 1)        // ワールド原点が NDC のどこに来るか
//      origin_texels = origin.xy * (kCascadeSize / 2)
//      offset = (round(origin_texels) - origin_texels) * (2 / kCascadeSize)
//      proj[3][0] += offset.x
//      proj[3][1] += offset.y
//      view_projection = proj * view
//      ★ スナップは proj の**平行移動成分**だけを動かす。proj 全体を作り直すのではない。
//      ★ 効果の確認: スナップを外してカメラをゆっくり平行移動すると、影の縁がチラチラ揺れる。
//        入れると揺れが止まる（縁はギザギザのまま、ただし動かない）。
//
// 5. texel_size = 2 * radius / kCascadeSize    // 法線オフセット用（D-8）
```

```cpp
// renderer.hpp に足す定数:
//   static constexpr std::uint32_t kCascadeCount        = 4;
//   static constexpr std::uint32_t kCascadeSize         = 2048;
//   static constexpr float         kShadowMaxDistance   = 100.0f;
//   static constexpr float         kCascadeSplitLambda  = 0.75f;
//   static constexpr float         kCascadeCasterMargin = 50.0f;
//   ★ phase16 の kShadowOrthoExtent / kShadowNear / kShadowFar / kShadowDistance は不要になる。
//     消し忘れても害は無いが、「どの定数が効いているのか」で迷う元になるので消すこと。
```

### ②-4. `CameraUBO` の拡張（[camera.hpp](../../engine/include/sq/scene/camera.hpp)）

```cpp
struct CameraUBO {
    glm::mat4  view_projection;                         // offset   0, 64
    glm::mat4  cascade_view_projection[kCascadeCount];  // offset  64, 256  ← light_view_projection を置き換え
    glm::vec4  camera_position;                         // offset 320, 16
    glm::vec4  camera_forward;                          // offset 336, 16  ← xyz = 視線方向（正規化済み）
    glm::vec4  cascade_splits;                          // offset 352, 16  ← 各段の遠い側のビュー深度
    glm::vec4  cascade_texel_sizes;                     // offset 368, 16  ← 各段の1テクセルのワールド幅（D-8）
    glm::uvec4 light_count;                             // offset 384, 16  x = ライト数 / y = デバッグフラグ（D-4）
};
static_assert(sizeof(CameraUBO) == 400, "std140 のレイアウトと一致させること");
```

- ★ `kCascadeCount` を `scene` 側から参照できる場所に置く必要がある（`camera.hpp` は `graphics` を知らない層）。`scene/shadow_constants.hpp` のような小さなヘッダに `inline constexpr std::uint32_t kCascadeCount = 4;` を置き、`Renderer` もそれを使う。GLSL 側は `camera.glsl` に `#define SQ_CASCADE_COUNT 4` を書き、**両方に「片方を変えたらもう片方も」とコメントを残す**。
- ★ std140 の `mat4` 配列は要素ごとに 64 バイト。`glm::mat4` の C 配列と一致する。
- ★ ⓪-1 で include 化してあるので、シェーダ側の修正は `camera.glsl` の1か所で済む。**⓪-1 をやっていなければ、ここで 5 本のシェーダを直すことになっていた。**

### ②-5. シャドウマップを4層にする

```
// shadow_maps_ の ShadowMapConfig を { .size = kCascadeSize, .layers = kCascadeCount,
//                                      .sample_view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY } にする。
// write_environment_sets の binding=0 はビューを sample_view() に変えるだけ（レイアウトは同じ）。
//
// ★ メモリ: 2048² × 4 バイト × 4 層 = 64 MiB / フレーム、× kFramesInFlight で 128 MiB。
//   GpuAllocator の kDefaultBlockSize（64 MiB）以上なので専用ブロックになる（create_block の
//   max(kDefaultBlockSize, reqs.size)）。動くが、重いと感じたら kCascadeSize を 1024 に下げる。
//   1024 でも 0 段目のテクセル密度は phase16 の数倍ある（②-11 の表）。
```

### ②-6. キャスタの収集（段ごとの視錐台）

```
// collect_draw_items:
//   const std::array<scene::Frustum, kCascadeCount> cascade_frusta = ...;  // 各段の view_projection から
//   ループの中で、半透明でなければ段ごとに intersects を判定し、cascade_lists_[c] へ push。
//
// ★ 1つの物体が複数の段に入るのは**正常**（境界をまたぐ物体、床のような大きな物体）。
//   床は4段すべてに入る。D-1 で kMaxInstances を上げた理由がこれ。
// ★ 段の視錐台は ②-3 の near の延長を含んだ行列から作る。輪切りそのものの視錐台で
//   カリングすると、延長した意味が無くなる（光源側の物体を収集の時点で捨ててしまう）。
// ★ ソートはメッシュ順（段ごと。phase16 と同じ）。
```

### ②-7. シャドウパスの記録

```
// record_shadow_pass:
//   方向光が無ければ**丸ごと飛ばす**（⓪-5 の初期遷移があるので、Begin/End も要らない）。
//
//   for c in 0..kCascadeCount:
//     BeginRenderPass(shadow_render_pass_, shadow_maps_[frame]->framebuffer(c), extent)
//     viewport / scissor（kCascadeSize）
//     vkCmdSetDepthBias(...)                    // 段ごとに値を変えてよい（D-8）
//     bind pipeline_shadow_ / set=0 / set=1
//     ShadowPushConstants pc{ .indices = { c, 0, 0, 0 } };
//     vkCmdPushConstants(cb, layout, VERTEX | FRAGMENT, 0, sizeof(pc), &pc)
//     record_draw_items(cascade_lists_[c])
//     EndRenderPass
//
// ★ パイプラインとディスクリプタのバインドはループの外に出してよい（レンダーパスを跨いでも残る）。
//   ただし vkCmdSetViewport / vkCmdSetDepthBias / vkCmdPushConstants は段ごとに必要。
// ★ サブパス依存（phase16 ⓪-1）は段ごとのパスそれぞれに効く。追加の変更は要らない。
```

```glsl
// shadow.vert:
#include "shadow_push.glsl"
gl_Position = camera.cascade_view_proj[pc.indices.x] * world_pos;
//   ★ light_view_proj は ②-4 で消える。古い名前のままだとコンパイルエラー（気付ける）。
```

### ②-8. 本パスでのサンプリング（[triangle.frag](../../shaders/triangle.frag)）

```glsl
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadow_cascades;
//   ★ sampler2DShadow → sampler2DArrayShadow。texture() の座標が vec3 → vec4
//     （uv, 層番号, 比較する深度）になる。

// 1.0 = 照らされている、0.0 = 完全な影。
float sample_cascade_shadow(vec3 world_pos, vec3 geo_normal, vec3 L) {
    // 1. ビュー深度で段を選ぶ
    float depth = dot(world_pos - camera.camera_position.xyz, camera.camera_forward.xyz);
    if (depth > camera.cascade_splits[SQ_CASCADE_COUNT - 1]) { return 1.0; }  // 影の最大距離の外
    uint c = 0;
    while (c < SQ_CASCADE_COUNT - 1 && depth > camera.cascade_splits[c]) { ++c; }

    // 2. 法線オフセット（D-8）。ジオメトリ法線を使うこと（法線マップの凹凸でずらすとぶれる）。
    //    角度の係数は sinθ（= 面が光に対してどれだけ傾いているか）。1 - NdotL ではない（D-8 の注記）。
    float texel = camera.cascade_texel_sizes[c];
    float NdotL = max(dot(geo_normal, L), 0.0);
    float sin_theta = sqrt(1.0 - NdotL * NdotL);
    vec3 offset_pos = world_pos + geo_normal * texel * kNormalOffsetScale * sin_theta;

    // 3. 光源クリップ空間へ → uv / z（phase16 ①-7 と同じ。z は再マップしない）
    // 4. 3x3 PCF: texture(shadow_cascades, vec4(uv + offset * texel_uv, float(c), z))
    return 1.0;  // TODO(②-8)
}
```

- ★ `geo_normal` は `normalize(frag_normal)`（法線マップ適用前）。phase16 ①-7 の注記「アクネが消えないときはジオメトリ法線で試す」を、ここで最初から採用する。
- ★ 段の境界で影の濃さ・ぼけ方が段差になる。境界付近で隣の段と混ぜる（ブレンド）のは将来課題。v1 は段差が見えることを**仕様**とする。

### ②-9. デバッグ表示（段の色分け）

```
// action.hpp: Action::ToggleShadowDebug を Count の前に足す。既定キーは F3。
// main.cpp:   押されたら renderer.set_shadow_debug(!renderer.shadow_debug()) 。
// Renderer:   bool shadow_debug_ を持ち、CameraUBO::light_count.y の bit0 に入れる。
// triangle.frag:
//   if ((camera.light_count.y & 1u) != 0u) { result *= kCascadeColors[c]; }
//   （赤・緑・青・黄。トーンマップの前に掛ける）
//
// ★ これが無いと「どの段で引いているか」が目で分からず、②-10 の調整がほぼ不可能になる。
//   面倒でも先に入れること。
```

### ②-10. bias の調整

```
// 出発点:
//   kDepthBiasConstant = 0.0f / kDepthBiasSlope = 1.5f（段共通）
//   kNormalOffsetScale = 1.5f（テクセル何個ぶんずらすか）
//
// ★ 調整の順序（phase16 ①-8 を段ごとに）:
//   1. デバッグ表示（②-9）をオンにして、床が4色に塗り分けられていることを確認する
//   2. 全部 0 にして縞が出ることを確認する（D-5 でカリングを NONE にしたので必ず出る）
//   3. kNormalOffsetScale を上げて縞を消す。**近い段（赤）と遠い段（黄）で同時に消えれば**
//      テクセル幅への比例（D-8）が効いている。片方だけ消えるなら texel_size の計算を疑う
//   4. 影が物体から浮くなら kNormalOffsetScale を下げ、slope で補う
//   5. **太陽の高さを3通り（低い・中くらい・ほぼ真上）変えて**、同じ値で縞が出ないことを確かめる
//      ★ main.cpp の太陽は Y 軸まわりにしか回らないので、高さは変わらない。向きを手で変えること。
//        角度の係数を誤ると（D-8 の 1 - NdotL）、ある高さでだけ消えて別の高さで戻る。
//
// ★ 縞の間隔 = その段の1テクセルの幅（2048 なら赤の段で約 4 mm、黄の段で約 7.5 cm）。
//   赤の段の縞は地面に 1〜2 m まで近づかないと見えず、遠い段の縞は画素より細い＋PCF で平均されて
//   「縞」ではなく「うっすらまだらに暗い」見え方になる。
//   → **調整中は kCascadeSize を 256 に下げる**（テクセルが 8 倍になり、普段の視点で全段の縞が見える）。
//     法線オフセットはテクセル幅の倍数なので、256 で決めた値は 2048 に戻してもそのまま効く
//     （戻して縞も浮きも出なければ、D-8 の比例が正しく効いている証拠になる）。
//   → 4色を近くに収めたいときは kShadowMaxDistance を一時的に 20 程度に下げる。
//   ★ 調整が終わったら両方とも元の値に戻すこと。
//
// 結果（phase17 実施時）:
//   - slope / constant は 0 で影が浮かなかった → 0 のまま（D-8 の注記）
//   - 係数を (1 - NdotL) にしていた間は、太陽の高さ約 35° で kNormalOffsetScale = 7.0 が必要だった
//     → sinθ に直して再調整する（2〜3 程度に下がる見込み）
```

### ②-11. 検証

- デバッグ表示で、カメラの近くから遠くへ**赤 → 緑 → 青 → 黄**と塗り分けられること。
- カメラの足元の影が phase16 より明らかに**細かい**こと。

| | 範囲（直径） | テクセル / ワールド 1 単位 |
|---|---|---|
| phase16（2048、±40） | 80 | 約 26 |
| ② 0 段目（2048、半径 ~3 として） | ~6 | 約 340 |
| ② 3 段目（2048、半径 ~70 として） | ~140 | 約 15 |

- カメラを**ゆっくり平行移動**しても影の縁が揺れないこと（テクセルスナップ）。スナップを一時的に外すと揺れることも確かめる。
- カメラを**その場で回転**しても影の縁が揺れないこと（境界球）。
- カメラから 100（`kShadowMaxDistance`）より遠い場所で影が消えること。これは**仕様**。
- 原点から 40 以上離れた場所でも、カメラが近ければ影が出ること（phase16 の制限の解消）。
- 高い物体の影が、カメラを物体の根元に近づけても消えないこと（`kCascadeCasterMargin`）。
- 太陽を回しても4段とも追随すること。

| 症状 | 最初に疑うところ |
|---|---|
| 影がまったく出ない | `cascade_splits` が 0（UBO に入れ忘れ）／ `camera_forward` がゼロベクトル |
| 色分けが同心円状になる | 段の選択に「距離」を使っている（②-2。ビュー深度を使う） |
| 段の境界で影がずれる | 段ごとの行列と層番号の対応がずれている（`cascade_view_proj[c]` と `vec4(uv, c, z)` の c） |
| カメラを動かすと影の縁がちらつく | テクセルスナップが効いていない（②-3 の 4） |
| カメラを回すと影の縁がちらつく | 境界球の半径を丸めていない ／ AABB で包んでいる |
| カメラを近づけると高い物体の影が消える | `kCascadeCasterMargin` が小さい ／ 段の視錐台を延長前の行列から作っている（②-6） |
| 遠い段だけ縞が出る | 法線オフセットが定数（テクセル幅に比例していない。D-8） |
| 太陽が高いときだけ縞が出る／縞を消すのに kNormalOffsetScale がやけに大きい | 角度の係数が `1 - NdotL`（sinθ であるべき。D-8） |
| 地面に近づかないと縞が見えない | 異常ではない（縞の間隔 = テクセル幅。②-10 の注記どおり kCascadeSize を下げて調整する） |
| 近い段だけ影が浮く | 同上（逆向き） |
| 画面全体が黒い | ②-3 の lookAt の up の縮退（太陽が真上）／ 輪切りの逆行列の z に -1 を使っている |

---

## ③ 点光源のキューブシャドウ

### ③-1. 点光源の選択と `LightData::shadow`

```
// collect_lights:
//   Point かつ cast_shadows == true のライトを、出てきた順に kMaxPointShadows 個まで選ぶ。
//   選んだライトの LightData::shadow = { float(slot), kPointShadowNear, light.range, 0 }。
//   5個目以降は警告して shadow.x = -1（phase16 D-4 と同じ扱い）。
//
//   FrameContext に足す:
//     std::uint32_t point_shadow_count = 0;
//     std::array<std::uint32_t, kMaxPointShadows> point_shadow_lights{};  // lights_ 内の添字
//
// ★ light.hpp の cast_shadows のコメント「Point で立てても無視される」を更新すること。
// ★ kMaxLights（64）でクランプされて消えたライトがスロットを持ったままにならないよう、
//   クランプの**後**でスロットを確定させること（phase16 で shadow_light_index に対してやったのと同じ）。
```

### ③-2. キューブ配列のシャドウマップ

```
// point_shadow_maps_（kFramesInFlight 個）:
//   ShadowMapConfig{ .size = kPointShadowSize, .layers = 6 * kMaxPointShadows,
//                    .sample_view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY }
//   層の番号 = slot * 6 + face
//
// renderer.hpp:
//   static constexpr std::uint32_t kMaxPointShadows = 4;
//   static constexpr std::uint32_t kPointShadowSize = 512;
//   static constexpr float         kPointShadowNear = 0.05f;
//   ★ メモリ: 512² × 4 バイト × 24 層 = 24 MiB / フレーム。
//
// device.cpp:
//   features.imageCubeArray = VK_TRUE（D-9）。
//   ★ supported_features.imageCubeArray を確認して、無ければ例外を投げる
//     （samplerAnisotropy のように「無ければ無効で続行」はできない。ビューが作れないため）。
//
// set=2 に binding=4 を足す:
//   create_descriptor_set_layout: env_bindings を 4 → 5 本
//   create_descriptor_pool:       COMBINED_IMAGE_SAMPLER を kMaxTextures + kFramesInFlight * 5 に
//   write_environment_sets:       binding=4 = { shadow_sampler_, point_shadow_maps_[i]->sample_view(),
//                                               DEPTH_STENCIL_READ_ONLY_OPTIMAL }
//   ★ 3箇所を同時に直すこと（phase16 ⓪-4 と同じ。プールを忘れると VK_ERROR_OUT_OF_POOL_MEMORY）。
//   ★ 比較サンプラは shadow_sampler_ をそのまま使ってよい。キューブのサンプリングでは
//     アドレスモードは実質無視される（面の境界は隣の面へシームレスに繋がる）。
```

### ③-3. 面ごとの行列

```
// 6面の view（eye = ライトの位置）。順序は Vulkan のキューブの層の順（+X, -X, +Y, -Y, +Z, -Z）。
//
//   面   注視方向      up
//   +X   ( 1, 0, 0)   (0,-1, 0)
//   -X   (-1, 0, 0)   (0,-1, 0)
//   +Y   ( 0, 1, 0)   (0, 0, 1)
//   -Y   ( 0,-1, 0)   (0, 0,-1)
//   +Z   ( 0, 0, 1)   (0,-1, 0)
//   -Z   ( 0, 0,-1)   (0,-1, 0)
//
// proj = perspective(radians(90), 1.0, kPointShadowNear, light.range)
// ★ **Y 反転をしない**（proj[1][1] *= -1 を書かない）。
//   キューブの各面のテクセルの並びは仕様で決まっていて（+X 面の1行目は +Y 側、など）、
//   「上の表の up + 反転なし」で描くと、Vulkan のフレームバッファの1行目（NDC y = -1）が
//   ちょうどその並びに一致する。カメラ・CSM と同じ感覚で反転を入れると、**影が上下に裏返る**。
// ★ 反転しないので三角形の巻き順は本パスと逆になるが、D-5 でカリングを NONE にしたので問題ない。
// ★ fov を 90° ちょうどにすること。90° より狭いと面の境界に隙間ができ、
//   **ライトの周りに十字形の影の抜け**が出る。
```

### ③-4. キャスタの収集

```
// ライトごとに:
//   1. 物体の境界球がライトの range の球と交わらなければスキップ（6面の判定を省く安い前判定）
//   2. 6面それぞれの視錐台（③-3 の proj * view から Frustum::from_view_projection）で判定し、
//      point_shadow_lists_[slot * 6 + face] へ push
//
// ★ 半透明は含めない（phase16 D-5 と同じ）。
// ★ 1つの物体が2〜3面に入るのは正常（面の境界をまたぐ）。
// ★ 点光源の range の内側に**ライト自身のメッシュ**（電球の球など）を置くと、自分の影で真っ暗になる。
//   今の検証シーンの点光源はメッシュを持たないので問題ないが、将来「光る球」を置くなら
//   そのエンティティだけキャスタから外す仕組み（Material か Light 側のフラグ）が要る。
```

### ③-5. シェーダ（新規 `shaders/shadow_point.vert` / `shadow_point.frag`）

```glsl
// shadow_point.vert:
//   gl_Position = pc.view_proj * world_pos;   // ★ CSM と違い、行列はプッシュ定数から（D-3）
//   frag_world_pos / frag_uv / frag_material_index を出力する
//
// shadow_point.frag:
//   1. ①-2 と同じ alpha の discard
//   2. LightData light = lights[pc.indices.y];
//      gl_FragDepth = length(frag_world_pos - light.position_type.xyz) / light.shadow.z;
//   ★ ① の discard の判定は shadow.frag と同じ式。material.glsl に関数として置いて両方から呼ぶ
//     （⓪-1 の include の使いどころ。コピーすると片方だけ直す事故が起きる）。
```

- ★ パイプライン `pipeline_point_shadow_`: `shadow_render_pass_` を使い、`PipelineConfig{ .has_color_attachment = false, .depth_bias_enable = false, .cull_mode = NONE }`。**`depth_bias_enable` を true にしても効かない**（D-9）。
- ★ `light.glsl` は set=0 binding=2 で、stageFlags は FRAGMENT。`shadow_point.frag` から読むので変更は要らない。

### ③-6. 記録

```
// record_point_shadow_pass:
//   for slot in 0..point_shadow_count:
//     light = lights_[point_shadow_lights[slot]]
//     for face in 0..6:
//       layer = slot * 6 + face
//       リストが空でも Begin/End は通す（★ 下記）
//       BeginRenderPass(framebuffer(layer)) → viewport / scissor（kPointShadowSize）
//       → bind pipeline_point_shadow_ / set=0 / set=1
//       → ShadowPushConstants{ .view_projection = face_vp, .indices = { 0, light_index, 0, 0 } }
//       → record_draw_items(point_shadow_lists_[layer]) → EndRenderPass
//
// ★ 使われているスロットの面は、**キャスタが0件でも Begin/End を通してクリアする**こと。
//   飛ばすと、前のフレーム（あるいは2フレーム前の同じ shadow_maps_）の深度が残り、
//   **動いた物体の影が残像として残る**。
//   使われていないスロット（slot >= point_shadow_count）は本パスが読まないので飛ばしてよい（⓪-5）。
//
// ★ パス数は最大 24。重ければ将来 multiview / ジオメトリシェーダで1パスにまとめる（D-2）。
```

### ③-7. 本パスでのサンプリング

```glsl
layout(set = 2, binding = 4) uniform samplerCubeArrayShadow point_shadows;

float sample_point_shadow(int slot, vec3 light_pos, float far, vec3 world_pos, vec3 geo_normal) {
    vec3 offset_pos = world_pos + geo_normal * kPointNormalOffset;   // ワールド単位（例 0.02）
    //   ★ ② と同じく、縞が角度で出たり消えたりするなら sinθ（sqrt(1 - NdotL²)）を掛ける（D-8）。
    //     ただし点光源のテクセル幅は「ライトからの距離 × 面1枚の画角 / 解像度」で場所ごとに変わるので、
    //     ② のように UBO から渡すのではなく、length(v) から求めるか、まず定数で通してから考える。
    vec3 v = offset_pos - light_pos;
    float ref = length(v) / far - kPointDepthBias;                   // 例 0.05 / far 相当
    return texture(point_shadows, vec4(v, float(slot)), ref);
    //   ★ texture(samplerCubeArrayShadow, vec4 P, float compare) の形。比較値は**第3引数**。
    //     sampler2DArrayShadow（vec4 の w に比較値）と形が違うので取り違えやすい。
    //   ★ v は正規化しなくてよい（キューブのルックアップは方向しか見ない）。
    //   ★ v の向きは「ライト → 表面」。逆にすると反対側の面を引いて影がでたらめになる。
}

// ライトのループ:
//   int slot = int(light.shadow.x);
//   if (slot >= 0) {
//       shadow = (type == 0) ? sample_cascade_shadow(...)
//                            : sample_point_shadow(slot, pos, light.shadow.z, ...);
//   }
```

- ★ PCF はまずハードウェアの 2×2（LINEAR の比較サンプラ）だけで通す。柔らかくしたければ、`v` の周りに小さなオフセット方向を足して数回引く（20 方向のディスクが定番）。将来課題でもよい。

### ③-8. 検証シーン（[main.cpp](../../sandbox_graphics/main.cpp)）

```
// create_point_light に cast_shadows 引数を足す（create_directional_light と同じく既定引数にしない）。
// 格子の中の点光源のうち4つ（色の違うもの）に cast_shadows = true を立てる。
//
// ★ 太陽の intensity を一時的に 0 にして、点光源の影だけを見る（phase16 ①-9 の「ライトを減らす」と同じ理由）。
// ★ 点光源の真下の床に、格子の箱の影が**放射状に**伸びれば正しい。
```

### ③-9. 検証

- 点光源の周りの物体が、ライトから**放射状に**影を落とすこと。
- ライトの真上・真下・前後左右の6方向すべてで影が出ること（面の向きの確認）。
- 面の境界（斜め45°の方向）で影が途切れないこと。
- 5つ目の `cast_shadows` 点光源で警告が出て、4灯だけが影を持つこと。
- 点光源の影と太陽の CSM が同時に正しく出ること。
- `cast_shadows` の点光源が 0 灯のとき、バリデーションが出ないこと（⓪-5 の初期遷移の確認）。
- 物体を動かしたとき、影に**残像が残らない**こと（③-6）。

| 症状 | 最初に疑うところ |
|---|---|
| 影が上下逆・左右逆に出る | 面の行列で Y 反転している ／ up の表の取り違え（③-3） |
| ライトの周りに十字形の抜けが出る | fov が 90° でない |
| 影がまったく出ない | `gl_FragDepth` を書いていない（透視の深度のまま）／ `shadow.z`（far）が 0 |
| 全面が影になる | 比較値を `texture()` の第3引数ではなく vec4 の w に入れている ／ `v` の向きが逆 |
| 縞が出る、bias を上げても消えない | `vkCmdSetDepthBias` で消そうとしている（`gl_FragDepth` では効かない。D-9） |
| 動いた物体の影が残る | キャスタ0件の面のパスを飛ばしている（③-6） |
| バリデーション「image layout」 | ⓪-5 の初期遷移が無い ／ 層ごとのビューの baseArrayLayer の取り違え |
| ビュー作成でバリデーション | `imageCubeArray` 未有効 ／ `CUBE_COMPATIBLE_BIT` が無い ／ layers が 6 の倍数でない |

---

## ④ エンティティの生成・破棄（`EntityCommands`）

**絵は1ピクセルも変わらない**（④-6 の検証シーンを除く）。①〜③と独立なので、いつ着手してもよい。

### ④-1. 現状の問題の整理

| 問題 | 今のコード |
|---|---|
| System から構造を変えられない | `System::update(const Registry&, float)`。`destroy` も `add` も呼べない |
| `each()` の中で構造を変えると壊れる | `View::each` は反復の最初に `count` を取り、`entities[i]` を順に読む。途中で swap-and-pop されると、**末尾の要素を飛ばすか、消えた要素を読む** |
| 親を消すと子が残る | `Registry::destroy` は子を知らない（`Parent` は子 → 親の片方向。phase14 ④）。子は死んだ親を指したまま、`TransformSystem` で単位行列扱いになり**原点へ飛ぶ** |
| 死んだハンドルで落ちる | `Registry::destroy` / `get` は死んだハンドルで例外を投げる。二重破棄で落ちる |

### ④-2. `EntityCommands`（新規 `engine/include/sq/ecs/entity_commands.hpp` / `src/ecs/entity_commands.cpp`）

```cpp
// エンティティの構造変更を溜めておき、同期点でまとめて実行する（phase17 ④ / D-10）。
//
// System は Registry を const で受け取り、構造を変えたいときはここへ積む。
// 実際の変更は SystemScheduler::update の末尾（全 System の後）で flush される。
class EntityCommands {
public:
    // そのエンティティだけを消す（子は残る。子孫ごと消すのは scene::destroy_hierarchy。④-4）。
    void destroy(Entity entity);

    // 同期点で fn(registry) を実行する。生成・コンポーネントの付け外しはこれで書く。
    //   ★ fn の中では registry を自由に変えてよい（もう反復中ではない）。
    //   ★ fn が捕捉した参照の寿命に注意。flush は同じ tick の中なので、
    //     ローカル変数の参照を捕捉しても tick を跨がなければ安全。
    void defer(std::function<void(Registry&)> fn);

    // 溜めたコマンドを積んだ順に実行して空にする。
    //   1. defer を積んだ順に実行
    //   2. destroy_ を重複除去し、is_alive のものだけ registry.destroy（④-4）
    //   ★ 破棄を最後にまとめる理由: defer が「消す予定のエンティティ」に add しても落ちないように。
    //     逆順だと、消したエンティティに add して例外になる。
    void flush(Registry& registry);

    [[nodiscard]] bool empty() const;

private:
    std::vector<Entity> destroy_;
    std::vector<std::function<void(Registry&)>> deferred_;
};
```

### ④-3. `System` / `ControlSystem` / `SystemScheduler` の変更

```cpp
// system.hpp
virtual void update(const Registry& registry, EntityCommands& commands, float dt) = 0;
// control_system.hpp
virtual void update(const Registry& registry, EntityCommands& commands,
                    const input::InputManager& input, const input::InputMap& map, float dt) = 0;

// system_scheduler.hpp
//   EntityCommands commands_; をメンバに持つ。
//   void update(Registry& registry, ...);   ★ const を外す（flush するため）
//     1. ControlSystem を回す（commands_ を渡す）
//     2. System を回す
//     3. commands_.flush(registry)          ← 同期点はここ1か所
//
// ★ System 自身に渡すのは const Registry& のまま。非 const にするのは Scheduler の入口だけ。
//   「System の中で直接 destroy できてしまう」状態を型で防ぐのが D-10 の要点。
// ★ CameraControlSystem の update のシグネチャも合わせて直すこと（commands は使わない）。
```

- ★ System の外（`main.cpp` の起動時や、ループ中のキー処理）からは、これまでどおり `registry` を直接変えてよい。**反復中でなければ**問題は無い。`EntityCommands` は「System の中から変えたい」ときの仕組み。

### ④-4. 階層を考慮した破棄（`scene::destroy_hierarchy`。[hierarchy.hpp](../../engine/include/sq/scene/hierarchy.hpp) にフリー関数として足す）

```cpp
// root とその全子孫の破棄を commands に積む（phase17 ④-4 / D-10）。
//   ★ 読むだけなので registry は const。実際に消えるのは flush のとき。
void destroy_hierarchy(const ecs::Registry& registry, ecs::EntityCommands& commands, ecs::Entity root);
```

```
// destroy_hierarchy の手順:
//   1. 子の表を作る:
//        std::unordered_map<Entity, std::vector<Entity>> children;
//        registry.view<Parent>().each([&](Entity e, Parent& p) { children[p.entity].push_back(e); });
//      （Entity の std::hash は entity.hpp に定義済み）
//   2. root から幅優先で子孫を集め、1体ずつ commands.destroy(e)
//   ★ 子は Parent で親を指す片方向リンクしか持たない（phase14 ④）ので、子孫を知るには
//     全 Parent を1回走査するしかない。呼ばれたときだけ走るので、毎 tick のコストにはならない。
//     同じ tick に何十回も呼ぶなら、表を1回だけ作って使い回す形に変える（将来の最適化）。
//
// EntityCommands::flush の破棄部分:
//   1. destroy_ を重複除去（sort + unique、または unordered_set）
//   2. is_alive(e) のものだけ registry.destroy(e)
//      ★ 死んだハンドルは**黙って飛ばす**（D-10）。二重に積まれるのは正常な使い方。
//
// ★ destroy（単体）で親だけ消した場合、子は残って原点へ飛ぶ（④-1）。これは仕様とし、
//   destroy のコメントに明記する。「子を残して親だけ消す」には、先に子の Parent を外す
//   （defer の中で registry.remove<Parent>）必要がある。
```

### ④-5. `Lifetime` コンポーネントと `LifetimeSystem`（新規 `scene/lifetime.hpp` / `lifetime_system.hpp` / `.cpp`）

```cpp
// 指定した秒数が経ったら自分（と子孫）を消す（phase17 ④-5）。
// ④ の最初の利用者であり、検証の道具でもある。
struct Lifetime {
    float remaining = 0.0f;  // 残り秒数
};

class LifetimeSystem final : public ecs::System {
public:
    // 1. view<Lifetime>().each で remaining -= dt
    //    ★ const Registry& から非 const の Lifetime& が取れるのは本エンジンの慣習（system.hpp の注記）。
    //      値の書き換えは構造変更ではないので、反復中でも安全。
    // 2. remaining <= 0 なら destroy_hierarchy(...)（④-4）で積む
    //    ★ ここで registry.destroy を直接呼べないのが D-10 の効果（const なのでコンパイルエラーになる）。
    void update(const ecs::Registry& registry, ecs::EntityCommands& commands, float dt) override;
};
```

### ④-6. 検証シーン: モデルの湧きと消滅（[main.cpp](../../sandbox_graphics/main.cpp)）

```
// 1. ModelSpawnerSystem（main.cpp 内のローカルな System でよい）:
//    kSpawnInterval 秒ごとに commands.defer で Duck を1体生成する。
//      defer([&model](Registry& r) {
//          root = r.create(); Transform / WorldTransform / Lifetime{ 3.0f } を付ける
//          spawn_model(r, model, root);
//      });
//    ★ Lifetime はルートにだけ付ける。消すときは LifetimeSystem が子孫ごと消す（④-4）。
//    ★ LoadedModel（Duck）は main が持ち続ける。エンティティだけが湧いては消える。
//
// 2. 1秒ごとに数をログに出す:
//      spdlog::info("entities={} meshes={} textures={} materials={}",
//                   registry.entity_count(), ...alive_count() ...);
//    ★ entities は「湧く数 × 寿命」で頭打ちになり、増え続けないこと。
//      meshes / textures / materials は**一切変わらない**こと（LoadedModel を持ち続けているため）。
//
// 3. LoadedModel そのもののアンロード（phase16 ③-6 の宿題）:
//    キー（Action を足すか、検証用に直接 GLFW キー）で ABeautifulGame を消す。
//      a. commands で destroy_hierarchy(model_root)（または main で直接 registry 操作）
//      b. flush の後で models から LoadedModel を erase する
//    ★ 順序は「エンティティ → LoadedModel」（phase16 ③-6）。逆にすると1フレーム市松模様。
//    ★ この時点で meshes / textures / materials が減ること。
//
// ★ main.cpp が握っているエンティティ（cameras の vector、sun）を消すときは、
//   使う前に is_alive を確認すること。get は死んだハンドルで例外を投げる。
```

### ④-7. 単体テスト（[tests/ecs_tests.cpp](../../tests/ecs_tests.cpp)）

ECS 側の変更なので、Catch2 のテストで固める（phase1 以来）。

- `destroy` を積んだだけでは消えず、`flush` で消えること。
- 同じエンティティを2回 `destroy` に積んでも落ちないこと。
- 既に死んだエンティティを積んでも落ちないこと。
- 3段の階層の根を `destroy_hierarchy` すると、3つとも消えること。兄弟の別の木は残ること。
- `defer` の中で `create` したエンティティが、`flush` の後に存在すること。
- `defer` と `destroy` を同じフレームに積んだとき、`defer` が先に走ること（④-2 の順序）。
- `each()` の中から積んだ `destroy` が、反復を壊さないこと（反復中に数えた件数が正しい）。

---

## 実装手順（この順で、各ステップ完了ごとに動作確認・コミット）

1. **⓪-1 シェーダの include 化** — `shaders/include/*.glsl` → CMake の `-I` と `DEPENDS` → **全シェーダが再コンパイルされ、絵が変わらないことを確認**（ここだけで1コミット）
2. **⓪-2 〜 ⓪-5 複数シャドウの土台** — `LightData::shadow` → プッシュ定数レンジ → `DrawList` → `ShadowMap` の層対応と初期遷移 → **絵が phase16 と完全に同じことを確認**
3. **① アルファマスク** — 本パスの `discard` → `shadow.frag` と set=1 → カリング NONE と bias → MASK の板で影が抜けることを確認
4. **②-1 〜 ②-8 CSM** — `FrameContext` のカメラパラメータ → 分割 → 段ごとの行列（スナップ込み）→ `CameraUBO` 400 バイト → 4層化 → キャスタ収集 → 記録 → frag のサンプリング
5. **②-9 〜 ②-11 CSM の調整と検証** — デバッグ表示（★ **調整より先に**）→ bias → シマリングが無いことを確認
6. **③ 点光源のキューブシャドウ** — 選択と `LightData::shadow` → キューブ配列と `imageCubeArray` → 面の行列 → キャスタ収集 → `shadow_point.*` → 記録 → frag のサンプリング → 放射状の影を確認
7. **④-1 〜 ④-4, ④-7 `EntityCommands`** — コマンドバッファ → Scheduler の同期点 → `destroy_hierarchy` → **単体テストを先に通す**
8. **④-5 〜 ④-6 `Lifetime` と検証シーン** — `LifetimeSystem` → 湧きと消滅で数が頭打ちになること → `LoadedModel` のアンロードで資産の数が減ること

> **⓪ の難所は「同期」（C++ とシェーダの定義を1か所に寄せる）、② の難所は「行列」、③ の難所は「向き」、④ の難所は「いつ消えるか」**。
>
> 1 と 2 は**絵が変わらないリファクタ**。phase16 と同じく、新機能と同じコミットに入れないこと。特に 1（include 化）は、以降のすべてのシェーダ変更の前提になる。
>
> ★ ② は 4 と 5 に分けてある。デバッグ表示が無い状態で bias を調整し始めると、「どの段の問題か」が分からないまま値をいじることになる。

---

## 検証観点（フェーズ全体）

- phase13〜16 の性能特性が**維持**されていること（不透明がメッシュ種類数のドローに畳まれる、フラスタムカリングが効く、半透明の順序が保たれる、IBL が効く）。
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab 復帰で、**CSM・点光源の影を含めて**描画が継続すること。
  - ★ `shadow_maps_` / `point_shadow_maps_` は `recreate_swapchain` で作り直さない（phase16 と同じ）。
- 終了時にバリデーションのリーク報告が無いこと。
- 影を落とすライトが **0 個**（方向光なし・点光源なし）で、バリデーションが出ないこと（⓪-5 の初期遷移の確認）。
- 方向光の影だけ・点光源の影だけ・両方、の3通りで正しく出ること。
- `kMaxInstances` を超える状況（格子を 17³ にするなど）で、**遠い影から**消えていくこと（D-1 の並び順の確認）。本体や近い影が欠けないこと。
- フレーム時間が phase16 から極端に伸びていないこと。シャドウパスは 1 → 最大 28 パスに増える。重ければまず `kPointShadowSize` と `kCascadeSize` を下げて切り分ける。
- エンティティを湧かせ続けても、エンティティ数・GPU メモリが頭打ちになること（④-6）。

---

## Phase 18 で行うこと（候補。phase17 完了時に確定させる）

1. **スポットライト** — 円錐角と内外の減衰。影は③の「キューブの1面ぶん」なので、点光源の配管にそのまま乗る（fov が円錐角になるだけ）。phase15 で「影とセットの方がやりやすい」と見送った項目。
2. **カスケードの境界のブレンド** — ②-8 で「段差が見えることを仕様」にした部分。境界付近で2段を引いて混ぜる。
3. **点光源の影の優先度** — D-9 で「最初の4灯」に固定した部分。カメラからの距離・画面への寄与で選び、スロットの割り当てが毎フレーム変わっても影が飛ばないようにする。
4. **シャドウパスの集約** — 点光源 24 パスを multiview（`VK_KHR_multiview`）か、ジオメトリシェーダの `gl_Layer` で1ライト1パスにまとめる（D-2 で見送った案）。
5. **ポストプロセスパス** — phase16 から継続。トーンマップを独立したパスへ出す。

## さらに先の将来課題

- **MASK とそれ以外でシャドウパイプラインを分ける**（D-6 で見送り。測ってから）。
- **ソフトシャドウの改善** — PCSS、VSM / ESM（phase16 から継続）。点光源の PCF（③-7）。
- **静的なライトのシャドウマップのキャッシュ** — 動かないライト・動かないキャスタなら毎フレーム描き直さない。
- **シェーダ依存の正確な追跡** — `glslc -MD` + CMake の `DEPFILE`（⓪-1 で「include を変えたら全部作り直す」にした部分）。
- **`doubleSided` マテリアルの対応** — glTF の両面フラグ。D-5 でシャドウは NONE になったが、本パスは BACK のまま。
- **`EntityCommands` での生成の仮ハンドル** — D-10 で見送った Bevy 型の `spawn`。
- **法線行列の事前計算**、**Forward+ / Clustered**、**専用キュー**、**VMA 差し替え**、**OIT**、**キーコンフィグの設定ファイル入出力**、**アニメーション / スキニング**（phase16 から継続）。
