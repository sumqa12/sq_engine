#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace sq::scene {

// 世代付きアセットハンドル（phase14 ② / D-5）。
//
// phase13 まで MeshId / TextureId は「レジストリ内の添字そのもの」だった。
// 登録しかしない（解放しない）うちはそれで足りるが、unload を入れた途端に破綻する:
//   1. TextureId 3 のテクスチャを unload する
//   2. スロット 3 が再利用され、別のテクスチャが登録される
//   3. 古い TextureId 3 を握ったままのエンティティが、**別の絵**で描かれる
// これが ABA 問題。添字だけでは「同じ場所」と「同じ物」を区別できない。
//
// → スロットごとに世代番号を持ち、解放のたびに +1 する。ハンドルは
//   { index, generation } の組で持ち、照合が合ったときだけ有効とみなす。
//
// ★ ecs::Entity が既に同じ仕組みを持っている（entity.hpp の id_ / generation_ と、
//   registry.hpp の EntityRecord::generation / free_ids_）。
//   **同じ設計をアセット側へ持ち込む**という理解でよい。実装はそちらを参考にすること。
struct AssetHandle {
    static constexpr std::uint32_t kInvalidIndex = ~0u;

    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] bool is_null() const { return index == kInvalidIndex; }

    friend bool operator==(const AssetHandle&, const AssetHandle&) = default;

    // ★ 不透明ソート（renderer.cpp）が「同じメッシュをまとめる」ために順序を要求する。
    //   ここで比べるのは index だけでよい（描画順に意味があるのは「同じ物か否か」だけで、
    //   generation の大小には意味が無い）。
    friend bool operator<(const AssetHandle& a, const AssetHandle& b) { return a.index < b.index; }
};

// 型で取り違えないよう、用途ごとに別型にする（強い typedef）。
//
// ★ 単なる using AssetHandle の別名だと、MeshId を受ける関数へ TextureId を渡しても
//   コンパイルが通ってしまう。継承して別型にすれば、その取り違えが型検査で落ちる。
// ★ 継承したぶん集成体初期化の書き方が変わる: MeshId{ { index, generation } } のように
//   基底ぶんの波括弧が要る（C++20 なら MeshId{ index, generation } でも通る）。
//   書きにくければ static な make 関数を足してもよい。
struct MeshId     : AssetHandle {};
struct TextureId  : AssetHandle {};
struct MaterialId : AssetHandle {};

}  // namespace sq::scene

// ★ std::unordered_map のキーにするなら特殊化が要る
//   （TextureRegistry::by_path_ は値側なので現状は不要だが、
//     phase14 ③ の glTF ローダーが「glTF内の添字 → ハンドル」の対応表を作るときに
//     逆引きが欲しくなる可能性がある）。
// ★ ecs::Entity の hash 特殊化（entity.hpp 末尾）と同じ形にしておくこと。
template <>
struct std::hash<sq::scene::AssetHandle> {
    std::size_t operator()(const sq::scene::AssetHandle& h) const noexcept {
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(h.index) << 32) | h.generation);
    }
};
