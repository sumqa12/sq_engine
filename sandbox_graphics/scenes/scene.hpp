#pragma once

#include <functional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "sq/assets/gltf_loader.hpp"
#include "sq/ecs/entity.hpp"
#include "sq/ecs/registry.hpp"
#include "sq/ecs/system_scheduler.hpp"
#include "sq/graphics/renderer.hpp"
#include "sq/scene/asset_handle.hpp"

// 検証用シーンの切り替え（phase17 ④の前に整理）。
//
// これまで main() に積み重ねていた phase13〜17 の検証用の空間を、検証テーマごとの「シーン」に分ける。
// 起動時のコマンドライン引数でシーン名を指定する（省略時は kDefaultSceneName）:
//   sandbox_graphics.exe shadows
//   sandbox_graphics.exe list      ← シーンの一覧を表示して終了
//
// ★ シーンの切り替えは**起動時だけ**。実行中に切り替えるには、エンティティの全破棄とアセットの解放が要るので、
//   phase17 ④（エンティティの生成・破棄）の上に作る（将来課題）。
//
// 構成:
//   scene.hpp / scene_registry.cpp   … シーンの一覧（名前 → 組み立て関数）と共通アセット
//   scene_helpers.hpp / .cpp         … カメラ・ライトの生成、Oscillator（どのシーンでも使う小道具）
//   scene_content.hpp / content_*.cpp … 検証テーマごとの「中身」（格子・glTF・マテリアルボール…）
// シーンは「中身」「照明」「カメラ」を組み合わせて作る。中身に照明を含めないのは、
// 全部入りのシーン（all）で太陽が2つにならないようにするため（2つ目の影を落とす方向光は無視されて警告が出続ける）。
namespace sandbox {

// 複数のシーンで共有するアセット（起動時に1回だけ登録する）。
struct CommonAssets {
    sq::scene::MeshId cube_mesh{};
    sq::scene::MeshId plane_mesh{};    // XZ 平面・法線 +Y・一辺 1
    sq::scene::MeshId sphere_mesh{};   // 半径 0.5
    sq::scene::TextureId default_texture{};          // 市松模様（読み込み失敗の可視化）
    sq::scene::TextureId white_texture{};            // 白 1×1（乗算の恒等元。base_color だけを出したいとき）
    std::vector<sq::scene::TextureId> icon_textures; // textures/hsr_icon_01 の全画像（RGBA）
};

// シーンを組み立てた結果のうち、main / loop() が後で使うもの。
struct SceneState {
    // 時間で回す方向光（phase16 ①-9）。null なら回さない。
    sq::ecs::Entity sun{};

    // glTF の所有権（phase16 ③-6）。破棄されるとメッシュ・マテリアル・テクスチャが解放される。
    // ★ SceneState は main() で **Renderer より後に宣言**すること（宣言の逆順に破棄されるので、
    //   Renderer のレジストリより先に LoadedModel が死ぬ）。逆だと死んだレジストリへ release して落ちる。
    // ★ アンロードは erase ではなく `models[i] = {};`（空の LoadedModel をムーブ代入）で行うこと（④-6）。
    //   erase すると後ろの要素がずれ、他の場所が持っている要素への参照・添字が壊れる。
    //   ムーブ代入なら、古い要素の AssetRef が release されて、要素の位置は変わらない。
    std::vector<sq::assets::LoadedModel> models;

    // 1秒ごとにエンティティ数とアセット数（メッシュ・テクスチャ・マテリアルの生存数）をログに出すか（phase17 ④-6）。
    //   エンティティを湧かせ続けても数が頭打ちになること、アンロードで資産が減ることの確認に使う。
    bool log_counts = false;

    // Action::SceneDebugAction（既定 F5）が押されたときに loop() が呼ぶ処理（phase17 ④-6）。null なら何もしない。
    //   lifecycle シーンでは「ABeautifulGame のエンティティを消してから LoadedModel をアンロードする」に使う。
    // ★ loop() の更新 tick の中、SystemScheduler::update の**外**で呼ばれる（反復中ではない）ので、
    //   registry を直接変えてよい。
    std::function<void(sq::ecs::Registry&)> on_debug_action;
};

// シーンを組み立てる関数に渡すもの一式。
struct SceneBuildContext {
    sq::ecs::Registry& registry;
    sq::graphics::Renderer& renderer;
    const CommonAssets& assets;
    std::mt19937& rng;    // 格子の回転・カメラの位置・点光源の色など（シーンをまたいで1本）
    SceneState& state;    // 組み立ての結果（sun / models）を書き込む先

    // シーン固有の System を登録する先（phase17 ④-6。lifecycle シーンの ModelSpawnerSystem など）。
    // ★ CameraControlSystem と LifetimeSystem は main() が全シーン共通で登録する。シーンは自分専用のものだけ足す。
    sq::ecs::SystemScheduler& scheduler;
};

using SceneBuildFunction = void (*)(SceneBuildContext& ctx);

struct SceneDefinition {
    std::string_view name;          // コマンドライン引数で指定する名前
    std::string_view description;   // 一覧表示用の説明
    SceneBuildFunction build;
};

// 引数を省略したときのシーン。phase17 ③ までの main() と同じ全部入り。
inline constexpr std::string_view kDefaultSceneName = "all";

// cube / plane / sphere のメッシュ、hsr_icon_01 の画像、既定・白テクスチャを登録する。
// ★ Renderer を作った直後、シーンを組み立てる前に1回だけ呼ぶ。
[[nodiscard]] CommonAssets register_common_assets(sq::graphics::Renderer& renderer);

// 登録されている全シーン（scene_registry.cpp の表）。
[[nodiscard]] std::span<const SceneDefinition> scene_definitions();

// 名前でシーンを探す。見つからなければ nullptr。
[[nodiscard]] const SceneDefinition* find_scene(std::string_view name);

// シーンの一覧（名前と説明）をログに出す。
void log_scene_list();

}  // namespace sandbox
