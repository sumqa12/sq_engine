# C++ ゲームエンジン（学習目的）— Phase 10: System抽象 + キーコンフィグ層 + Orbitスキーム + アクティブカメラ選択 + SingleTimeCommands

## Context
Phase 9で入力システム（`InputManager` + `Controller`/`ControlTarget` + FreeFlyカメラ + 右ドラッグ視線）まで到達した（[phase9-input-system.md](phase9-input-system.md)参照）。現状は以下の「暫定形」が残っている:

- 入力処理は `sq::input::update_camera_control(registry, input, dt)` という**free関数**。System抽象がないため、更新処理を増やすたびに `main.cpp` のループが肥大化する。
- キーは `GLFW_KEY_W` 等の**物理キー定数を直接** `input_system.cpp` に埋め込んでいる。キーの割り当て変更にはエンジン側の再コンパイルが必要。
- 操作スキームは `ControlScheme::FreeFly` の1種類のみ（enumは用意済みだが分岐がない）。
- アクティブカメラは `registry.view<Camera>().front()`（**アーキタイプ走査順で最初に見つかったもの**）。カメラを増やすと、どれが使われるか制御できない。
- 使い捨てコマンドバッファ（プール生成→記録→submit→`vkQueueWaitIdle`→解放）が `texture.cpp` のコンストラクタに**べた書き**。plan11でDEVICE_LOCAL転送を実装すると同じコードが再出現する。

Phase 10ではこの5点を解消し、Phase 11（描画側の最適化）に進む前に土台を整える。

例によってClaudeは宣言・骨格・TODOコメントまで、実装本体はユーザーが書く（CLAUDE.md）。

---

## 設計判断（本セッションで確定）

### D-1. System抽象は2つの基底クラスに分ける
- **`sq::ecs::System`**: 入力を必要としない更新処理の基底。`update(const Registry&, float dt)`
- **`sq::ecs::ControlSystem`**: 入力を必要とする更新処理の基底。`update(const Registry&, const InputManager&, const InputMap&, float dt)`
- 両者は**継承関係にしない**（シグネチャが異なるため。`System`を継承させて片方をfinalで潰す設計より、2本立ての方が意図が明快）。
- `SystemScheduler` が両方を所有し、**全ControlSystemに同一の `InputManager`/`InputMap` 参照を渡す**（ユーザー指定の要件）。
  スケジューラが参照を1組だけ保持するので、システム側が個別にInputManagerを持つ必要がなくなる。

**`registry` を `const&` で受ける理由**: 本エンジンの慣習（`View::each()` は const registry からコンポーネントの非const参照を渡す。`Renderer::draw_frame` も同様）。エンティティの生成・破棄を伴うシステムが必要になったら非constに変える（将来課題）。

**配置**: `System` / `ControlSystem` / `SystemScheduler` はいずれも `sq::ecs`（`engine/include/sq/ecs/`）に置く。
`ControlSystem` は `sq::input::InputManager` / `InputMap` に依存するが、**ヘッダでは前方宣言のみ**とし（参照を素通しするだけなので定義は不要）、ECSヘッダがinputモジュールを引き込まないようにする。
将来 input 以外の依存（時間・アセット等）が増えたら `sq::app` 層へ切り出す（将来課題）。

### D-2. キーコンフィグ層は `enum class Action` + バインディング表
- `Action` は**エンジンが提供する抽象操作**（MoveForward / LookEnable / ToggleFullscreen ...）。
- `InputMap` が `Action → 物理キー/ボタン` の対応表を持ち、`InputManager` に問い合わせて解決する。
- **`InputMap` のヘッダはGLFW非依存を維持**する（`InputManager` と同じ方針）。`GLFW_KEY_*` 定数が登場するのは
  `input_map.cpp` の `default_map()` の中だけにする。これによりキーコンフィグ層が「GLFW → エンジン抽象」の
  変換点として1箇所に閉じる。
- 1アクションに**複数バインド可**（例: MoveUp = Space、LookEnable = 右ボタン or 左Alt）。固定長配列で持ち、動的確保しない。

### D-3. Orbitスキームを追加
`ControlScheme::Orbit` を実装し、`Controller::scheme` で分岐する。FreeFlyと状態（yaw/pitch）を共有し、
Orbit固有のパラメータ（注視点までの距離）を `Controller` に追加する。

### D-4. アクティブカメラは `ActiveCamera` タグ
- `struct ActiveCamera {}`（データメンバなしのタグ）を付けたカメラを描画に使う。
- 選択の優先順位（`Renderer::draw_frame`）:
  1. `view<Camera, ActiveCamera>().front()` — タグ付きカメラ
  2. `view<Camera>().front()` — タグなしなら従来通り最初のカメラ（**後方互換フォールバック**）
  3. どちらも無ければ `Camera::default_view_projection(aspect)`（既存の挙動）
- 切替ヘルパ `sq::scene::set_active_camera(Registry&, Entity)` を用意する。
  **注意（ECSの罠）**: `view().each()` のラムダ内で `add`/`remove` するとアーキタイプ間移動が起きて
  反復中のストレージが壊れる。**先に対象Entityを`std::vector`へ収集し、ループを抜けてから付け替える**。

### D-5. SingleTimeCommands は RAIIクラス
記録→送信→完了待ち→解放を1つのオブジェクトに閉じ込める。`texture.cpp` のべた書きを置き換え、
plan11のDEVICE_LOCAL転送でそのまま再利用する。

---

## A. System抽象（入力を正式なECS Systemへ昇格）

### A-1. 新規 `engine/include/sq/ecs/system.hpp`（ヘッダオンリー）

```cpp
namespace sq::ecs {

// 入力に依存しない更新処理の基底クラス。SystemScheduler が毎更新tickで update を呼ぶ。
// dt は前回updateからの経過秒。
class System {
public:
    virtual ~System() = default;

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    virtual void update(const Registry& registry, float dt) = 0;

protected:
    System() = default;  // 抽象基底。派生からのみ構築する
};

}  // namespace sq::ecs
```

### A-2. 新規 `engine/include/sq/ecs/control_system.hpp`（ヘッダオンリー）

```cpp
// InputManager / InputMap は参照を素通しするだけなので前方宣言で足りる（ECSヘッダをGLFW/inputから独立に保つ）。
namespace sq::input { class InputManager; class InputMap; }

namespace sq::ecs {

// 入力を必要とする更新処理の基底クラス。
// SystemScheduler が保持する「同一の」InputManager / InputMap 参照を全ControlSystemへ渡す。
class ControlSystem {
public:
    virtual ~ControlSystem() = default;

    ControlSystem(const ControlSystem&) = delete;
    ControlSystem& operator=(const ControlSystem&) = delete;

    virtual void update(const Registry& registry,
                        const input::InputManager& input,
                        const input::InputMap& map,
                        float dt) = 0;

protected:
    ControlSystem() = default;
};

}  // namespace sq::ecs
```

### A-3. 新規 `engine/include/sq/ecs/system_scheduler.hpp` / `src/ecs/system_scheduler.cpp`

```cpp
namespace sq::ecs {

// System / ControlSystem を所有し、登録順に update を回す。
// 実行順序: ControlSystem（入力反映）→ System（それ以外）。
// 入力で変化した状態を同じtick内でゲームロジックが読めるようにするため、この順に固定する。
class SystemScheduler {
public:
    SystemScheduler() = default;
    ~SystemScheduler() = default;

    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    // 所有権を受け取る。戻り値は登録したシステムへの参照（呼び出し側でパラメータ調整に使う）。
    template <typename T, typename... Args> T& emplace_system(Args&&... args);
    template <typename T, typename... Args> T& emplace_control_system(Args&&... args);

    // 1更新tick分を実行する。input/map は全ControlSystemで共有される。
    void update(const Registry& registry,
                const input::InputManager& input,
                const input::InputMap& map,
                float dt);

private:
    std::vector<std::unique_ptr<ControlSystem>> control_systems_;
    std::vector<std::unique_ptr<System>> systems_;
};

}  // namespace sq::ecs
```

- `emplace_*` はテンプレートなのでヘッダに定義を置く（`static_assert(std::derived_from<T, ControlSystem>)` を付けて
  誤った型の登録をコンパイル時に弾くと学習になる。`<concepts>`）。
- `update()` の本体は `.cpp`（前方宣言だけでは呼べないため、`.cpp` で `input_manager.hpp`/`input_map.hpp` をinclude）。

### A-4. 新規 `engine/include/sq/input/camera_control_system.hpp` / `src/input/camera_control_system.cpp`

既存の free 関数 `update_camera_control` を `ecs::ControlSystem` の派生クラスへ昇格させる。

```cpp
namespace sq::input {

// ControlTarget + Controller + Camera を持つエンティティに入力を適用する。
// Controller::scheme に応じて FreeFly / Orbit を切り替える。
class CameraControlSystem : public ecs::ControlSystem {
public:
    CameraControlSystem() = default;

    void update(const ecs::Registry& registry, const InputManager& input,
                const InputMap& map, float dt) override;

private:
    // yaw/pitch から前方ベクトルを求める（両スキーム共通）。
    [[nodiscard]] static glm::vec3 forward_from(float yaw, float pitch);

    // 視線回転（LookEnable 押下中のカーソルデルタを yaw/pitch に反映し pitch をクランプ）。両スキーム共通。
    static void apply_look(const InputManager& input, const InputMap& map, scene::Controller& controller);

    static void update_free_fly(const InputManager& input, const InputMap& map,
                                scene::Controller& controller, scene::Camera& camera, float dt);
    static void update_orbit(const InputManager& input, const InputMap& map,
                             scene::Controller& controller, scene::Camera& camera, float dt);
};

}  // namespace sq::input
```

- **移行**: `input_system.hpp` / `input_system.cpp`（free関数）は**削除**し、CMakeのソース一覧からも外す。
  既存のFreeFly実装本体は `update_free_fly` へそのまま移す（ロジックは変更しない。キー問い合わせだけ `map` 経由に置換）。
- 既存実装のバグ相当の点（移行時に直すこと）: 現状は視線回転の条件が `if (!input.is_down(GLFW_KEY_LEFT_ALT))`
  で**常時視線が動く**（Altを押している間だけ止まる）挙動になっている。Phase 9プランの意図は
  「LookEnable 押下中のみ視線を動かす」なので、`map.is_down(input, Action::LookEnable)` が **true のとき**に
  回転させる形へ改める（カーソルキャプチャの切替条件と同じ向きに揃える）。

---

## B. キーコンフィグ層

### B-1. 新規 `engine/include/sq/input/action.hpp`（ヘッダオンリー）

```cpp
namespace sq::input {

// エンジンが提供する抽象操作。物理キーとの対応は InputMap が持つ。
// 末尾の Count は配列サイズ用の番兵（新しい Action は必ず Count の前に足す）。
enum class Action {
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    LookEnable,        // 押下中のみ視線回転（マウス右ボタン等）
    ToggleFullscreen,
    Quit,
    Count,
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

}  // namespace sq::input
```

### B-2. 新規 `engine/include/sq/input/input_map.hpp` / `src/input/input_map.cpp`

```cpp
namespace sq::input {

// バインド先のデバイス種別。キーとマウスボタンは番号空間が別なので区別が要る。
enum class InputDevice { Keyboard, Mouse };

struct InputBinding {
    InputDevice device = InputDevice::Keyboard;
    int code = -1;  // GLFW_KEY_* / GLFW_MOUSE_BUTTON_*。-1 は未割り当て
};

// Action → 物理キー/ボタンの対応表（キーコンフィグ層）。
// InputManager と同じくGLFW非依存（このヘッダにGLFW定数は現れない）。
// GLFW定数が登場するのは default_map() の実装（.cpp）だけ。
// 1アクションに複数バインドでき（例: LookEnable = 右ボタン or 左Alt）、いずれかが成立すれば真。
class InputMap {
public:
    static constexpr std::size_t kMaxBindingsPerAction = 4;

    InputMap() = default;

    // 既定のキーコンフィグを返す（WASD移動 / Space・LeftCtrl 昇降 / 右ボタン視線 / F11 / ESC）。
    [[nodiscard]] static InputMap default_map();

    // 割り当て。bind は空きスロットへ追加、set は既存を消してから1つ設定。
    void bind(Action action, InputDevice device, int code);
    void set(Action action, InputDevice device, int code);
    void clear(Action action);

    // 解決付き問い合わせ。いずれかのバインドが成立すれば真。
    [[nodiscard]] bool is_down(const InputManager& input, Action action) const;
    [[nodiscard]] bool is_pressed(const InputManager& input, Action action) const;
    [[nodiscard]] bool is_released(const InputManager& input, Action action) const;

    // 現在の割り当ての参照（設定UI・保存処理で使う想定）。
    [[nodiscard]] std::span<const InputBinding> bindings_of(Action action) const;

private:
    // 添字は static_cast<std::size_t>(Action)。未割り当てスロットは code = -1。
    std::array<std::array<InputBinding, kMaxBindingsPerAction>, kActionCount> bindings_{};
};

}  // namespace sq::input
```

実装メモ（TODOに書く内容）:
- `is_down/is_pressed/is_released` は3つとも「バインドを走査して、device で `input.is_*` と `input.is_mouse_*` を
  呼び分け、いずれか true なら true」という同型の処理。**述語を受ける private ヘルパ1本にまとめる**とよい
  （`bool any_binding(const InputManager&, Action, KeyFn, ButtonFn) const` 等）。
- `default_map()` の中身: MoveForward=`GLFW_KEY_W` / MoveBackward=`GLFW_KEY_S` / MoveLeft=`GLFW_KEY_A` /
  MoveRight=`GLFW_KEY_D` / MoveUp=`GLFW_KEY_SPACE` / MoveDown=`GLFW_KEY_LEFT_CONTROL` /
  LookEnable=`GLFW_MOUSE_BUTTON_RIGHT`（Mouse） / ToggleFullscreen=`GLFW_KEY_F11` / Quit=`GLFW_KEY_ESCAPE`。
- 設定ファイル（JSON等）からの読み込みは**将来課題**。今回はコード上の `default_map()` + `set()` による差し替えまで。

---

## C. Orbitスキーム

### C-1. `engine/include/sq/scene/controller.hpp` の拡張

```cpp
enum class ControlScheme {
    FreeFly,  // WASDで平行移動、マウスで視線回転（カメラ位置が主）
    Orbit,    // 注視点(Camera::target)を中心に周回、WASDで注視点をパン、ホイールで距離ズーム
};

struct Controller {
    ControlScheme scheme = ControlScheme::FreeFly;
    float move_speed = 3.0f;
    float mouse_sensitivity = 0.0025f;
    float yaw = 0.0f;
    float pitch = 0.0f;

    // -- Orbit 用 --
    float orbit_distance = 5.0f;      // 注視点からカメラまでの距離
    float zoom_speed = 0.5f;          // ホイール1ノッチあたりの距離変化
    float min_orbit_distance = 1.0f;  // ズームのクランプ範囲
    float max_orbit_distance = 50.0f;
};
```

### C-2. `update_orbit` の手順（TODOに明記する内容）

1. `apply_look()` で yaw/pitch を更新（FreeFlyと共通。LookEnable押下中のみ）
2. `orbit_distance -= scroll_delta * zoom_speed;` → `min/max_orbit_distance` にクランプ
3. `forward = forward_from(yaw, pitch)`、`right = normalize(cross(forward, world_up))`
4. 移動アクションで**注視点（`camera.target`）をパンする**。水平面に投影した前方
   （`normalize(vec3(forward.x, 0, forward.z))`）と `right` を使う。MoveUp/MoveDown は world_up 方向
   （Orbitでは「注視点の上下移動」になる。FreeFlyの「カメラの上下移動」とは意味が違う点をコメントで説明）
5. **カメラ位置を注視点から逆算**: `camera.position = camera.target - forward * orbit_distance;`
   - FreeFlyが「position を動かして target を追従させる」のに対し、Orbitは「target を動かして position を
     逆算する」— 主従が逆になるのが両スキームの本質的な違い。ここを学習ポイントとしてコメントに書く
6. 真上・真下でのジンバルロック回避のため pitch のクランプは共通ヘルパ（`apply_look`）で行う

### C-3. スキームの切替
`Controller::scheme` を書き換えるだけ。切替時に位置が飛ぶのを避けたい場合は、
Orbitへ入る瞬間に `orbit_distance = length(camera.position - camera.target)` を再計算するとよい
（**今回はやらない**。切替UIを作る段階で対応する）。

---

## D. アクティブカメラの選択ロジック

### D-1. `engine/include/sq/scene/camera.hpp` に `ActiveCamera` タグを追加

```cpp
// 描画に使うカメラを示すタグ（データメンバなし）。
// 複数のカメラエンティティがあっても、このタグが付いたものが Renderer に選ばれる。
// タグ付きが複数ある場合はアーキタイプ走査順の最初の1つ（決定的ではないので、
// set_active_camera() で常に1つだけになるよう管理する）。
struct ActiveCamera {};

// entity を唯一のアクティブカメラにする。他のエンティティからは ActiveCamera を外す。
// 注意: view().each() のラムダ内で add/remove するとアーキタイプ移動で反復が壊れるため、
// 「先に対象Entityをvectorへ収集 → ループを抜けてから付け替え」の2段構えで実装すること。
void set_active_camera(sq::ecs::Registry& registry, sq::ecs::Entity entity);
```

- `set_active_camera` の実装は [camera.cpp](../../engine/src/scene/camera.cpp) に置く（`registry.hpp` のincludeが必要）。
- `entity` が `Camera` を持たない場合の扱い: `has<Camera>()` で確認し、持たなければ warn ログを出して何もしない
  （throwはしない。学習用サンドボックスで落としたくないため）。

### D-2. `renderer.cpp` の `draw_frame` を3段フォールバックへ

現状（177〜189行付近）:
```cpp
ecs::Entity camera_entity = registry.view<scene::Camera>().front();
glm::mat4 view_projection = scene::Camera::default_view_projection(aspect_ratio);
if (!camera_entity.is_null()) { ... }
```
を、
1. `registry.view<scene::Camera, scene::ActiveCamera>().front()`
2. null なら `registry.view<scene::Camera>().front()`（後方互換）
3. それも null なら `default_view_projection`

に変更する。**フォールバックした事実は毎フレームログを出さない**（1回だけ、または出さない）。

### D-3. `sandbox_graphics/main.cpp`
カメラエンティティ生成時に `registry.add<ActiveCamera>(camera_entity, {})` を追加する
（`set_active_camera` を使ってもよい）。動作確認用に**2つ目のカメラ**（別位置・別パラメータ）を作り、
キーでタグを付け替えて切り替わることを確認できるようにする（切替キーは `Action` へ足さず、
サンドボックス側で生キー問い合わせでよい。恒久的な操作なら `Action` に追加する）。

---

## E. SingleTimeCommandsヘルパの共通化

### E-1. 新規 `engine/include/sq/graphics/single_time_commands.hpp` / `src/graphics/single_time_commands.cpp`

```cpp
namespace sq::graphics {

// 1回きりの転送コマンド（レイアウト遷移・バッファ/イメージコピー等）を実行するためのRAIIヘルパ。
// コンストラクタで TRANSIENT な一時コマンドプールと1本のコマンドバッファを作り、記録可能な状態
// （vkBeginCommandBuffer 済み・ONE_TIME_SUBMIT）にする。
// handle() に記録し、submit_and_wait() で end → submit → vkQueueWaitIdle まで行う。
// デストラクタは submit_and_wait() 未呼び出しなら呼び出してから、バッファとプールを破棄する。
//
// 注意: vkQueueWaitIdle でCPUをブロックする（起動時のアセット転送用。毎フレームの描画では使わない）。
class SingleTimeCommands {
public:
    SingleTimeCommands(VkDevice device, std::uint32_t queue_family, VkQueue queue);
    ~SingleTimeCommands();

    SingleTimeCommands(const SingleTimeCommands&) = delete;
    SingleTimeCommands& operator=(const SingleTimeCommands&) = delete;

    // 記録先。vkCmd* をこれに対して呼ぶ。
    [[nodiscard]] VkCommandBuffer handle() const;

    // 記録を終えて送信し、完了まで待つ。2回目以降の呼び出しは何もしない。
    void submit_and_wait();

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
    bool submitted_ = false;
};

}  // namespace sq::graphics
```

実装メモ（TODOに書く内容）:
- コンストラクタ: `VkCommandPoolCreateInfo{ flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queueFamilyIndex }`
  → `vkCreateCommandPool` → `vkAllocateCommandBuffers`（PRIMARY, 1本）
  → `vkBeginCommandBuffer`（`VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`）。各戻り値をチェックしてthrow。
  **途中でthrowする場合は、それまでに作ったプールを破棄してから投げる**（コンストラクタが例外で抜けると
  デストラクタは呼ばれない — RAIIの重要な学習ポイント）。
- `submit_and_wait()`: `submitted_` なら即return → `vkEndCommandBuffer` → `vkQueueSubmit` → `vkQueueWaitIdle`
  → `submitted_ = true`。
- デストラクタ: `submit_and_wait()` → `vkFreeCommandBuffers` → `vkDestroyCommandPool`。
  **デストラクタからは例外を投げない**こと（`submit_and_wait` 内のVkResultチェックは、失敗時に
  spdlogへerror出力するに留める設計にするか、明示呼び出し用と分ける）。

### E-2. `texture.cpp` の置き換え
コンストラクタ内の「一時プール生成〜プール破棄」（現在の87〜139行付近）を以下に置き換える:

```cpp
SingleTimeCommands cmd(device, graphics_queue_family, graphics_queue);
transition_image_layout(cmd.handle(), image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
vkCmdCopyBufferToImage(cmd.handle(), staging_buffer.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
transition_image_layout(cmd.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
cmd.submit_and_wait();
```

ついでに掃除する点（`texture.cpp` の既存の粗）:
- 末尾の `(void)physical_device; (void)graphics_queue_family; (void)graphics_queue; (void)path;` は
  すべて実際に使われている引数なので**削除**する。
- `stbi_load` 失敗時のフォールバック（`textures/default.png` 再読込）で、**再読込も失敗した場合のnullチェックが無い**。
  `StagingBuffer` に nullptr を渡して `memcpy` するとクラッシュするため、throw するガードを足す。
- `vkBindImageMemory` 失敗時のメッセージが「vkAllocateMemory : メモリの確保に失敗しました。」のままなので直す。
- `stbi_image_free(pixels)` の呼び出しが見当たらない（StagingBufferへコピー後に解放すること）。**リーク修正**。

### E-3. plan11への布石
`SingleTimeCommands` は plan11 の「頂点/インデックスバッファのDEVICE_LOCAL化」で
`vkCmdCopyBuffer`（StagingBuffer → DEVICE_LOCALバッファ）にそのまま使う。今回はTextureのみの置き換えに留める。

---

## F. `sandbox_graphics/main.cpp` の整理

現在の更新tick内の直書き処理を、スケジューラ + キーコンフィグに置き換える。

```
// 起動時
sq::input::InputManager input;
sq::input::InputMap input_map = sq::input::InputMap::default_map();
sq::ecs::SystemScheduler scheduler;
scheduler.emplace_control_system<sq::input::CameraControlSystem>();
// （将来）scheduler.emplace_system<MovementSystem>();

// 更新tick内
if (input_map.is_pressed(input, Action::Quit))             { break; }
if (input_map.is_pressed(input, Action::ToggleFullscreen)) { renderer.set_fullscreen(!renderer.is_fullscreen()); }
renderer.set_cursor_captured(input_map.is_down(input, Action::LookEnable));
scheduler.update(registry, input, input_map, du);
input.new_frame();
```

- `printf("F11\n")` のデバッグ出力を削除する。
- `glfwTerminate()` を直接呼んでからbreakしている箇所は、**`Window` のデストラクタが `glfwTerminate` を
  呼ぶ前提と二重になっていないか確認**する（`renderer` はこのスコープのローカルなので、break後の
  デストラクタで正規の破棄経路を通る。GLFWを先に落とすとVulkanのサーフェス破棄と順序が競合しうる）。
  → `break` だけにして、後片付けはRAIIに任せる形へ直す。
- `delta_u_time` / `delta_f_time` が計算されているが未使用（コンパイラ警告の温床）。削除するか実際に使う。

---

## G. CMake変更

`engine/CMakeLists.txt` の `add_library` ソース一覧:
- **追加**: `src/ecs/system_scheduler.cpp`, `src/input/input_map.cpp`, `src/input/camera_control_system.cpp`,
  `src/graphics/single_time_commands.cpp`
- **削除**: `src/input/input_system.cpp`（free関数版。`CameraControlSystem` へ移行するため）

ヘッダオンリー（`system.hpp` / `control_system.hpp` / `action.hpp`）はソース一覧に足さない。

---

## 本セッションでの実施結果（骨格作成・完了）

CLAUDE.mdのルール（コード生成は宣言まで）に従い以下を作成。**骨格はすべてコンパイル可能な構成**にしてあり、
全ターゲットのビルド成功・`ctest` 全パスを確認済み。新規クラスはTODOスタブのため、
**ユーザー実装完了までは従来通りの挙動**（free関数版のFreeFly操作・最初のカメラを使用・texture.cppはべた書きのまま）。

- **新規（System抽象）**:
  [system.hpp](../../engine/include/sq/ecs/system.hpp)、[control_system.hpp](../../engine/include/sq/ecs/control_system.hpp)（ヘッダオンリー。
  input系は前方宣言のみ）、[system_scheduler.hpp](../../engine/include/sq/ecs/system_scheduler.hpp)（`emplace_*` は
  `requires std::derived_from<...>` 付きで定義済み）/ [system_scheduler.cpp](../../engine/src/ecs/system_scheduler.cpp)（`update` はTODO）
- **新規（キーコンフィグ）**: [action.hpp](../../engine/include/sq/input/action.hpp)（`Action` + `kActionCount` + `action_index`。完成済み）、
  [input_map.hpp](../../engine/include/sq/input/input_map.hpp) / [input_map.cpp](../../engine/src/input/input_map.cpp)（`bindings_of` のみ実装、他はTODO）
- **新規（カメラ操作システム）**: [camera_control_system.hpp](../../engine/include/sq/input/camera_control_system.hpp) /
  [camera_control_system.cpp](../../engine/src/input/camera_control_system.cpp)（`update` / `apply_look` / `update_free_fly` / `update_orbit` がTODO）
- **新規（転送ヘルパ）**: [single_time_commands.hpp](../../engine/include/sq/graphics/single_time_commands.hpp) /
  [single_time_commands.cpp](../../engine/src/graphics/single_time_commands.cpp)（TODOスタブ）
- **コンポーネント変更**:
  - [controller.hpp](../../engine/include/sq/scene/controller.hpp): `ControlScheme::Orbit` と Orbit用4パラメータを追加（完成済み）
  - [camera.hpp](../../engine/include/sq/scene/camera.hpp): `ActiveCamera` タグと `set_active_camera()` 宣言を追加、`registry.hpp` をinclude
  - [camera.cpp](../../engine/src/scene/camera.cpp): `set_active_camera()` を2段構え（収集→付け替え）の手順TODO付きで追加
- **TODOコメント追加（既存実装は保持）**:
  - [renderer.cpp](../../engine/src/graphics/renderer.cpp): アクティブカメラ3段フォールバック
  - [texture.cpp](../../engine/src/graphics/texture.cpp): `SingleTimeCommands` への置き換え＋既存の粗（リーク・nullチェック・
    エラーメッセージ・不要な `(void)`）の修正指示
  - [sandbox_graphics/main.cpp](../../sandbox_graphics/main.cpp): スケジューラ/InputMap生成、Actionベースへの置換、
    `glfwTerminate()` 直呼びの除去、`ActiveCamera` 付与と2台目カメラ
- **ビルド設定**: [engine/CMakeLists.txt](../../engine/CMakeLists.txt) に `system_scheduler.cpp` / `input_map.cpp` /
  `camera_control_system.cpp` / `single_time_commands.cpp` を追加。`input_system.cpp` は移行完了後に削除する旨をTODOで明記（現状は残置）

---

## 実装順序（推奨）

1. **`SingleTimeCommands`**（E）— 他と独立。`texture.cpp` を置き換えて、テクスチャが従来通り表示されることを確認。
   `texture.cpp` の粗（リーク・nullチェック）もここで潰す
2. **`Action` + `InputMap`**（B）— 既存の free 関数 `update_camera_control` の中身を `map` 経由に置き換え、
   FreeFlyが従来通り動くことを確認（この時点ではまだSystem化しない）
3. **System抽象**（A）— `System` / `ControlSystem` / `SystemScheduler` を作り、`CameraControlSystem` へ移植。
   `input_system.hpp/cpp` を削除、`main.cpp` をスケジューラ方式へ
4. **`ActiveCamera`**（D）— タグ追加 → `renderer.cpp` の3段フォールバック → main で2台目のカメラを作って切替確認
5. **Orbit**（C）— `Controller` にOrbitパラメータ追加 → `update_orbit` 実装 → スキーム切替で挙動が変わることを確認

1〜2は互いに独立なので順序を入れ替えてもよい。3は2の完了後（`map` 経由の問い合わせが済んでから）が楽。

---

## 検証方法

- ビルドとテストが通ること（**CLion同梱のcmake/ctestを使う**。buildツリーがそれで構成されているため、
  別バージョンのcmakeでは再構成に失敗する）。
  複数デバイスで開発しているため、cmakeの絶対パスとbuildツリー名はここに固定で書かない。
  その環境での実際の値は `CMakeCache.txt` の `CMAKE_COMMAND` を見るのが確実。
- テストはECSコアの既存ケースが全パスすること（ECSコアは非変更。System抽象は新規追加のみ）
- 手順1完了時: 立方体にテクスチャが従来通り貼られ、検証レイヤーのエラーが出ないこと
- 手順2完了時: WASD / Space / LeftCtrl / 右ボタン視線 / F11 / ESC が従来通り効くこと。
  `InputMap::set(Action::MoveForward, Keyboard, GLFW_KEY_UP)` 等で割り当てを差し替えると挙動が変わること
- 手順3完了時: `main.cpp` のループから入力ロジックが消え、`scheduler.update(...)` 1行になっていること。
  更新tick(30UPS)と描画(240FPS)のカデンツ差があっても移動が滑らかなこと（`is_down` ベースのため）
- 手順4完了時: カメラを2台置き、`ActiveCamera` を付け替えると視点が切り替わること。
  タグを全部外すと従来通り最初のカメラが使われ、カメラを全部消しても落ちずに既定ビューになること
- 手順5完了時: `scheme = Orbit` のカメラで、右ドラッグすると注視点を中心に周回し、
  ホイールで距離が変わり、WASDで注視点がパンすること。真上・真下でジンバルロックしないこと
- リサイズ・最小化復帰・フルスクリーン切替(F11)・Alt+Tab復帰で、入力とカメラ操作が継続すること

---

## Phase 11 で行うこと（このフェーズではやらない・確定事項）

以下の3項目は **plan11** で実装する。いずれも描画パイプラインとGPUメモリ管理の最適化で、
Phase 10の入力・シーン側の整理とは独立している。

1. **半透明の描画順ソート（back-to-front）／不透明・半透明パスの分離**
   - Phase 8.1でアルファブレンドを有効化したが、`depthWriteEnable = VK_TRUE` のまま未ソートで描いているため
     視点角度によって背後が正しく透けない（[phase8-texture-mapping.md](phase8-texture-mapping.md) の
     「3. 深度書き込みと描画順」参照）。
   - 不透明パス（depthWrite=TRUE）→ 半透明パス（depthWrite=FALSE、カメラからの距離で降順ソート）の
     2パス構成へ。パイプラインを2本持つか、動的ステートで切り替えるかも含めて設計する。
   - Phase 10 の `ActiveCamera` が確定していることで「どのカメラからの距離でソートするか」が一意に決まる。

2. **頂点/インデックスバッファのDEVICE_LOCAL化（`StagingBuffer` が土台）**
   - 現在 `VertexBuffer` / `IndexBuffer` は `HOST_VISIBLE | HOST_COHERENT` に直書きしている。
     `StagingBuffer`（TRANSFER_SRC）→ `vkCmdCopyBuffer` → DEVICE_LOCAL バッファへ移行する。
   - Phase 10 で作る `SingleTimeCommands` をそのまま転送に使う。

3. **VMA導入／サブアロケーション（`small-dedicated-allocation` 警告への対応）**
   - Phase 7で観測した性能警告（小さなバッファごとに個別 `vkAllocateMemory` している）への対応。
   - vcpkg の `vulkan-memory-allocator` を導入するか、自前で単純なサブアロケータを書くかを選ぶ
     （学習目的としては「まず自前で1つ書いてからVMAに置き換える」も有力）。
   - `Buffer` 基底クラスのメモリ確保部分が唯一の変更点になるよう、Phase 6の共通化が効いてくる。

## さらに先の将来課題（plan12以降）

- ミップマップ生成（`vkCmdBlitImage`）
- Meshコンポーネント化（エンティティごとに別メッシュ/テクスチャ）、複数テクスチャ・マテリアル
- 専用トランスファーキュー（キューファミリ跨ぎの所有権移譲）
- キーコンフィグの設定ファイル入出力（JSON等）。`Action` ⇔ 文字列の変換表が必要
- エンティティの生成・破棄を行うSystem（`Registry&` を非constで受ける形への拡張）
- `sq::app` 層への `SystemScheduler` 切り出し（input以外の依存が増えたとき）
- 操作対象の切替UI・Orbit⇔FreeFly切替時の状態引き継ぎ
