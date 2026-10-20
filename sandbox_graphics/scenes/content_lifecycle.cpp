#include "scene_content.hpp"

#include <cstddef>
#include <random>

#include <glm/gtc/quaternion.hpp>

#include "sq/assets/gltf_loader.hpp"
#include "sq/assets/model_spawner.hpp"
#include "sq/ecs/entity_commands.hpp"
#include "sq/ecs/system.hpp"
#include "sq/scene/hierarchy.hpp"   // WorldTransform / destroy_hierarchy
#include "sq/scene/lifetime.hpp"
#include "sq/scene/transform.hpp"

using namespace sq::scene;

namespace sandbox {

namespace {

// 一定間隔でモデルを湧かせ、Lifetime を付けて数秒後に消えるようにする System（phase17 ④-6。sandbox 専用）。
//
// ★ System なので registry は const。生成は commands.defer の中で行う（D-10）。
//   defer の中では registry を自由に変えてよい（同期点で実行され、もう反復中ではない）。
class ModelSpawnerSystem final : public sq::ecs::System {
public:
    // model: 湧かせる glTF。★ 参照を持つので、SceneState::models の要素を指すこと。
    //   models に後から emplace_back すると再確保で参照が壊れるので、**全モデルを読み込み終えてから**作ること。
    //   アンロードは erase ではなく `models[i] = {}` で行う約束（scene.hpp）なので、位置はずれない。
    //   ★ ただし、この model 自体をアンロードすると、空の LoadedModel を spawn_model することになる
    //     （nodes が空なので何も湧かないだけで、落ちはしない）。
    ModelSpawnerSystem(const sq::assets::LoadedModel& model, std::mt19937& rng);

    // 1. timer_ += dt。kSpawnInterval を超えたら timer_ -= kSpawnInterval して1体湧かせる
    // 2. 湧かせる位置をランダムに決め（x, z ∈ [-kSpawnRange, kSpawnRange]、y = 0）、
    //    commands.defer([this, position](Registry& r) { ... }) を積む:
    //      root = r.create(); Transform（position, 単位 quat, kModelScale）/ WorldTransform / Lifetime{ kLifetime } を付ける
    //      spawn_model(r, *model_, root)
    //    ★ Lifetime は**ルートにだけ**付ける。消すときは LifetimeSystem が子孫ごと消す（④-4 / ④-5）。
    //    ★ ラムダに this を捕捉してよい（System は SystemScheduler が所有し、flush は同じ tick の中で走る）。
    //    ★ 乱数は defer の**外**（update の中）で引いて、値で捕捉すること。
    void update(const sq::ecs::Registry& registry, sq::ecs::EntityCommands& commands, float dt) override;

private:
    static constexpr float kSpawnInterval = 1.0f;   // 何秒ごとに1体湧かせるか
    static constexpr float kLifetime      = 3.0f;   // 1体が何秒で消えるか（→ 同時に居るのは約 3 体）
    static constexpr float kSpawnRange    = 8.0f;   // 湧く範囲（原点からの半幅）
    static constexpr float kModelScale    = 2.0f;

    const sq::assets::LoadedModel* model_ = nullptr;  // 所有しない（SceneState::models が所有）
    std::mt19937* rng_ = nullptr;                     // 所有しない（main() が所有）
    float timer_ = 0.0f;
};

ModelSpawnerSystem::ModelSpawnerSystem(const sq::assets::LoadedModel& model, std::mt19937& rng)
    : model_(&model), rng_(&rng) {}

void ModelSpawnerSystem::update(const sq::ecs::Registry& registry, sq::ecs::EntityCommands& commands, float dt) {
    timer_ += dt;
    if (timer_ >= kSpawnInterval) {
        timer_ -= kSpawnInterval;
        std::uniform_real_distribution spawn_dist(-kSpawnRange, kSpawnRange);
        glm::vec3 position(spawn_dist(*rng_), 0, spawn_dist(*rng_));
        commands.defer([this, position](sq::ecs::Registry& r) {
            sq::ecs::Entity root = r.create();
            r.add<Transform>(root, Transform{position, glm::quat(1, 0, 0, 0), glm::vec3(kModelScale)});
            r.add<WorldTransform>(root, WorldTransform{});
            r.add<Lifetime>(root, Lifetime{ kLifetime });
            [[maybe_unused]] const auto model_entities = spawn_model(r, *model_, root);
        });
    }
}

}  // namespace

void add_lifecycle_test(SceneBuildContext& ctx) {
    //   Duck と ABeautifulGame を ctx.state.models に読み込む（load_gltf。content_gltf.cpp と同じ呼び方）
    auto& registry = ctx.registry;
    auto& renderer = ctx.renderer;

    struct ModelFile {
        std::string_view path;
        float scale;
    };
    constexpr ModelFile kModels[] = {
        { "models/Duck.glb",           2.0f },
        { "models/ABeautifulGame.glb", 10.0f },
    };
    constexpr std::size_t kDuckIndex = 0;
    constexpr std::size_t kGameIndex = 1;

    for (auto [path, scale] : kModels) {
        ctx.state.models.emplace_back(
            sq::assets::load_gltf(std::string(path), renderer.meshes(), renderer.textures(), renderer.materials()));
    }
    //   ★ 2つとも読み込み終えてから、要素への参照・添字を取ること（emplace_back の再確保で参照が壊れる）。
    //     添字（duck_index / game_index）を覚えておき、参照は最後に ctx.state.models[i] で取るのが安全。

    //   ABeautifulGame を1つ置く（空の親 game_root の下に spawn_model。位置は湧く範囲の外、例: (20, 0, 0)、スケール 10）
    const sq::ecs::Entity game_root = registry.create();
    registry.add<Transform>(game_root, Transform(
        glm::vec3(20, 0, 0), glm::quat(1, 0, 0, 0), glm::vec3(10)));
    registry.add<WorldTransform>(game_root, WorldTransform{});

    [[maybe_unused]] const auto model_entities = sq::assets::spawn_model(registry, ctx.state.models[kGameIndex], game_root);

    //   スポーンシステムの追加
    ctx.scheduler.emplace_system<ModelSpawnerSystem>(ctx.state.models[kDuckIndex], ctx.rng);

    ctx.state.on_debug_action = [&state = ctx.state, game_root](sq::ecs::Registry& r) {
        sq::ecs::EntityCommands cmds;
        destroy_hierarchy(r, cmds, game_root);
        cmds.flush(r);
        state.models[kGameIndex] = {};
        //   ★ 順序は「エンティティ → LoadedModel」（phase16 ③-6）。逆にすると、エンティティが死んだハンドルを
        //     握ったまま1フレーム描かれ、既定メッシュ／既定マテリアル（市松模様）へフォールバックした姿が一瞬見える。
        //   ★ 2回目の F5 では game_root が既に死んでいる。destroy_hierarchy は何もせず、models[game_index] も空なので
        //     空を空で上書きするだけ（何も起きない）でよい。
        //   ★ GPU がまだそのフレームのバッファを使っている心配は要らない。メッシュ・テクスチャの解放は
        //     DeletionQueue が kFramesInFlight フレーム遅らせる（phase14 ②-4）。
        //   ★ state を参照で捕捉してよい（SceneState は main() の最後まで生きている）。
    };

    ctx.state.log_counts = true;
}

}  // namespace sandbox
