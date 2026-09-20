#pragma once

#include "sq/scene/asset_handle.hpp"

namespace sq::scene {

// テクスチャの識別子。実体は graphics::TextureRegistry が所有する（phase12 D-1）。
// Texture はコピー禁止のRAII型なので、コンポーネントはIDのみを持つ（MeshId と同じ方針）。
// phase14 ②: { index, generation } の scene::TextureId（asset_handle.hpp）へ移行した。
inline constexpr TextureId kInvalidTextureId{};

// マテリアルの識別子（phase14 ①）。実体（graphics::MaterialData）は
// graphics::MaterialRegistry が所有する SSBO の配列要素で、**index** が添字になる。
// ★ シェーダへ渡すのは handle.index だけ（generation は CPU 側の検証用。D-5）。
inline constexpr MaterialId kInvalidMaterialId{};

// 描画マテリアル（ECSコンポーネント。phase11 ① で新設、phase14 ① で痩せた）。
//
// phase13 までは albedo / base_color を**体ごとに**持っていたが、
// 見た目のパラメータは graphics::MaterialData（GPU側の共有配列）へ移した。
// ここに残るのは「どのマテリアルを使うか」と「どのパスで描くか」だけ。
//
// Material を持たないエンティティは既定マテリアル
// （既定テクスチャ・白・不透明）として扱う（後方互換）。
struct Material {
    // 参照するマテリアル。kInvalidMaterialId または未登録IDなら
    // MaterialRegistry::default_material() が使われる。
    MaterialId id = kInvalidMaterialId;

    // true なら半透明パスで back-to-front 描画する（phase11 ①）。
    //
    // ★ これは MaterialData に入れない。CPU 側の描画パス振り分けにしか使わず、
    //   シェーダへは渡らないため（GPU に送っても読む者がいない）。
    //   glTF の alphaMode == "BLEND" を読んだときはここを立てる（phase14 ③）。
    bool transparent = false;
};

}  // namespace sq::scene
