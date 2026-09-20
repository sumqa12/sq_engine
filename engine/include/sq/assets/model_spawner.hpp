#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sq/assets/gltf_loader.hpp"
#include "sq/ecs/entity.hpp"
#include "sq/ecs/registry.hpp"

namespace sq::assets {

// LoadedModel を ECS のエンティティ階層として展開する（phase14 ③-5）。
//
// ローダー（load_gltf）と分けてあるのは、
//   「ロードは1回・配置は何回でも」を成立させるため。
// 同じ LoadedModel を引数に何度呼んでも、GPU リソースは共有されたまま体だけが増える。
//
//
// root_parent: 生成したルート群の親にするエンティティ。null なら親を付けない。
//   ★ ここに Transform を持つ空エンティティを渡しておけば、
//     そのスケール・位置を動かすだけでモデル全体が動く（④ の階層の成果が効く場所）。
//
// 戻り値: model.nodes と同じ並びの Entity 配列（呼び出し側が個別のノードを掴めるように）。
[[nodiscard]] std::vector<ecs::Entity> spawn_model(ecs::Registry& registry,
                                                   const LoadedModel& model,
                                                   ecs::Entity root_parent = {});

}  // namespace sq::assets
