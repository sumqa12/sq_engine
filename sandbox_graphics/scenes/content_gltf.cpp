#include "scene_content.hpp"

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include <glm/gtc/quaternion.hpp>

#include "sq/assets/gltf_loader.hpp"    // glTF 読み込み（phase14 ③）
#include "sq/assets/model_spawner.hpp"  // LoadedModel → エンティティ展開（phase14 ③）
#include "sq/scene/hierarchy.hpp"       // WorldTransform（phase14 ④）
#include "sq/scene/transform.hpp"

using namespace sq::scene;

namespace sandbox {

void add_gltf_models(SceneBuildContext& ctx) {
    auto& registry = ctx.registry;
    auto& renderer = ctx.renderer;

    // ---- phase14 ③: glTF モデルの読み込みと配置 ----
    // ★ assets/models/ の中身が、ビルド後に実行ファイルの隣の models/ へコピーされる
    //   （sandbox_graphics/CMakeLists.txt の POST_BUILD）。パスは実行時CWD基準。
    struct ModelFile {
        std::string_view path;
        float scale;
    };
    constexpr ModelFile kModels[] = {
        { "models/Duck.glb",           2.0f },
        { "models/ABeautifulGame.glb", 10.0f },
        { "models/Box.glb",            2.0f },
    };

    for (std::size_t i = 0; i < std::size(kModels); ++i) {
        const auto& [path, scale] = kModels[i];

        // ★ LoadedModel は ctx.state.models が所有する（破棄されるとアセットが解放される。phase16 ③-6）。
        auto& model = ctx.state.models.emplace_back(
            sq::assets::load_gltf(std::string(path), renderer.meshes(), renderer.textures(), renderer.materials()));

        // モデル全体をまとめて動かすための空の親を1つ作る（④ の階層の使いどころ）
        const float f = i % 2 == 0 ? 5.0f : -5.0f;
        const sq::ecs::Entity model_root = registry.create();
        registry.add<Transform>(model_root, Transform(
            glm::vec3(static_cast<float>(i) * f, 0, 0), glm::quat(1, 0, 0, 0), glm::vec3(scale)));
        registry.add<WorldTransform>(model_root, WorldTransform{});

        [[maybe_unused]] const auto model_entities = sq::assets::spawn_model(registry, model, model_root);
    }
    //   ★ ctx.state.models は vector なので、emplace_back で再確保が起きても LoadedModel はムーブされるだけ
    //     （AssetRef のムーブは参照カウントを変えない）。spawn_model に渡しているのは再確保の前の参照ではなく、
    //     emplace_back が返した直後の参照なので安全。
}

}  // namespace sandbox
