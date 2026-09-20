#pragma once

#include "sq/scene/asset_handle.hpp"

namespace sq::scene {

// メッシュ（ジオメトリ）の識別子。実体は graphics::MeshRegistry が所有する（phase12 D-1）。
// VertexBuffer / IndexBuffer はコピー禁止のRAII型であり、本エンジンのECSは add<T> 時に
// 値をストレージへmoveするため、実体をコンポーネントに入れるとアーキタイプ間移動で壊れる。
// そのためコンポーネントはIDのみを持つ。
//
// phase14 ②: 添字そのもの（std::uint32_t）から { index, generation } の
// scene::MeshId（asset_handle.hpp）へ移行した。既定構築が無効値になる。
inline constexpr MeshId kInvalidMeshId{};

// このエンティティが描画するジオメトリ（ECSコンポーネント）。
// MeshHandle を持たないエンティティは描画されない
// （カメラ等の非描画エンティティを描画対象から除外する判定に使う）。
struct MeshHandle {
    MeshId id = kInvalidMeshId;
};

}  // namespace sq::scene
