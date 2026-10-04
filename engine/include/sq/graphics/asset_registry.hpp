#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sq::graphics {

// 3つのアセットレジストリ（Mesh / Texture / Material）の共通部分（phase16 ③-2 / D-10）。
//
// phase14 ②-2 のコメント（material_registry.hpp）に書いたとおり、
// 「3つとも書いてみてから共通項を抜く」という順序でここへ来た。
// 3つの差分は「登録時の副作用」と「解放時の処理」の2つだけ（③-1 の表）なので、
// 仮想関数2本（on_registered / on_destroy）で吸収する。
//
// Handle: scene::MeshId / TextureId / MaterialId（scene::AssetHandle の派生）
// Entry:  スロットに置く実体。**ムーブ可能であればよい**（コピー不可でよい）。
//         ★ ただし既定構築可能であること（slots_.emplace_back() で空のスロットを作るため）。
//
// ★ 手順7（このステップ）では参照カウントを**入れない**。
//   既存の unload() をそのまま destroy() に載せ替えるところまで（絵が変わらないリファクタ）。
//   retain / release / ref_count と Slot::ref_count は手順8（③-4）で足す。
//
// ★ テンプレートなのでヘッダオンリー（.cpp は作らない。CMake の変更も不要）。
//   メンバ関数の定義はこのヘッダの末尾（クラス定義の外）に書くこと。
//   クラス外定義の書き方（テンプレート基底を書くのが初めてなら、ここで一度つまずく）:
//
//     template <typename Handle, typename Entry>
//     bool AssetRegistry<Handle, Entry>::contains(Handle id) const { ... }
//
//   ★ 戻り値の型にクラス内の型を使うときは typename が要る（依存名のため）:
//     template <typename Handle, typename Entry>
//     typename AssetRegistry<Handle, Entry>::Slot& AssetRegistry<Handle, Entry>::...
//   ★ 派生クラス（MeshRegistry 等）は**テンプレートではない**ので、
//     基底のメンバを使うときに this-> を付ける必要は無い
//     （this-> が要るのは「派生側もテンプレート」のときだけ）。
template <typename Handle, typename Entry>
class AssetRegistry {
public:
    virtual ~AssetRegistry() = default;

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // id が有効な登録済みハンドルか（phase14 ②。ABA 問題の検出）。
    //   既存の3つの contains() と同じ3条件:
    //     id.index < slots_.size() && slots_[id.index].alive && slots_[id.index].generation == id.generation
    //   ★ 3条件すべてが要る。alive を落とすと「解放済みスロットを指す古いハンドル」が、
    //     generation を落とすと「再利用されたスロットを指す古いハンドル」が素通りする。
    //   ★ Handle は値で受ける（8 バイトなので参照にする意味が無い）。
    [[nodiscard]] bool contains(Handle id) const {
        return id.index < slots_.size()
        && slots_[id.index].alive
        && slots_[id.index].generation == id.generation;
    }

    // 範囲外／世代不一致は例外（先に contains で確認する）。
    //   MeshRegistry::get の実装をそのまま持ってくればよい。
    [[nodiscard]] const Entry& get(Handle id) const {
        if (contains(id)) {
            return slots_[id.index].entry;
        }

        throw std::runtime_error("AssetRegistry::get : 登録されていません。");
    };

    // 生きているスロット数（検証・デバッグ用。③-7）。
    //   ★ slots_.size() ではない（解放済みスロットも含むため）。
    //     slots_.size() - free_indices_.size() で求まる。
    [[nodiscard]] std::size_t alive_count() const {
        return slots_.size() - free_indices_.size();
    };

protected:
    // capacity: 登録できる件数の上限。0 なら無制限（MeshRegistry）。
    //   TextureRegistry は max_textures（ディスクリプタ配列の枠）、
    //   MaterialRegistry は max_materials（SSBO の要素数）を渡す。
    explicit AssetRegistry(std::uint32_t capacity = 0) : capacity_(capacity) {};

    // これ以上登録できないか。
    //   capacity_ != 0 && slots_.size() >= capacity_ && free_indices_.empty()
    //   ★ 解放済みスロットがあるなら上限に達していても登録できる（既存の判定と同じ）。
    //
    // ★ 派生の add() / load() は**実体を作る前に**これを見ること。
    //   既存の挙動（上限なら spdlog::error を出して既定テクスチャ／既定マテリアルを返す）は
    //   派生側で維持する。emplace() の例外は「確認を忘れた」ことを知らせる最後の砦。
    //   ★ 先に見ないと、ディスクから画像を読んで GPU へ転送した後で捨てることになる。
    [[nodiscard]] bool full() const {
        return capacity_ != 0
        && slots_.size() >= capacity_
        && free_indices_.empty();
    };

    // スロットを確保して Handle を払い出す（派生の add() / load() が実体を作り終えてから呼ぶ）。
    //   1. full() なら std::runtime_error を投げる（③-2。上限チェックをここへ集約する）
    //   2. free_indices_ が空でなければ末尾から取り出して再利用、空なら slots_.emplace_back()
    //   3. slot.entry = std::move(entry)
    //   4. generation は**インクリメントしない**（上げるのは destroy 時。ここで上げると
    //      発行した直後のハンドルが自分自身と照合しなくなる）
    //   5. alive = true にして Handle{ { index, slot.generation } } を作る
    //   6. on_registered(id, slot.entry) を呼んでから id を返す
    //
    // ★ 既存の TextureRegistry::load は「スロット確保 → Texture 生成」の順だったため、
    //   Texture のコンストラクタが例外を投げると emplace_back したスロットが
    //   alive でも free_indices_ でもない宙ぶらりんになっていた。
    //   「実体を作り終えてから emplace」の順にすると、この穴も同時に塞がる。
    [[nodiscard]] Handle emplace(Entry entry) {
        if (full()) {
            throw std::runtime_error("AssetRegistry::emplace : 登録件数の上限に達しています。");
        }

        std::uint32_t index;
        if (free_indices_.empty()) {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        } else {
            index = free_indices_.back(); // 解放済みスロットを再利用
            free_indices_.pop_back();
        }

        Slot& slot = slots_[index];
        slot.entry = std::move(entry);
        slot.alive = true;

        Handle id = Handle{{ index, slot.generation }};
        on_registered(id, slot.entry);

        return id;
    }

    // 即座に解放する（既存の unload() の共通部分）。
    //   1. contains(id) でなければ何もしない（二重解放の防止）
    //   2. on_destroy(id, std::move(slot.entry)) を呼ぶ
    //      ★ alive を落とす**前**に呼ぶこと。on_destroy の中では id がまだ有効なので、
    //        派生側は contains / get を普通に使える（TextureRegistry が既定テクスチャの
    //        view を取りに行くのはこのタイミング）。
    //   3. slot.entry = Entry{} で空にする
    //      ★ on_destroy がムーブで持っていかなかった場合（MaterialRegistry）に備える。
    //        手順8で MaterialEntry に TextureRef が入ると、ここで**テクスチャの release が走る**。
    //   4. alive = false, ++generation
    //   5. free_indices_.push_back(id.index)
    //
    // ★ 「既定アセットは解放させない」判定はここに入れない（派生ごとに既定の有無が違う）。
    //   派生の unload() が destroy() を呼ぶ前に判定する。
    void destroy(Handle id) {
        if (!contains(id)) { return; }

        Slot& slot = slots_[id.index];
        on_destroy(id, std::move(slot.entry));

        slot.entry = Entry{};

        slot.alive = false;
        ++slot.generation;

        free_indices_.push_back(id.index);
    };

    // 登録済みの実体を書き換え用に取り出す（MaterialRegistry::update 用）。
    //   contains(id) でなければ例外（get と同じ）。
    // ★ slots_ を派生へ直接見せない代わりの窓口。slots_ を protected にすると、
    //   派生が alive / generation / free_indices_ を直接いじれてしまい、
    //   括り出した意味（スロット管理を1箇所に閉じる）が薄れる。
    [[nodiscard]] Entry& get_mutable(Handle id) {
        if (contains(id)) {
            return slots_[id.index].entry;
        }

        throw std::runtime_error("AssetRegistry::get_mutable : 登録されていません。");
    };

    // -- 派生が埋めるフック --

    // emplace() の最後に呼ばれる。
    //   TextureRegistry  : bindless 配列の id.index 番目へ vkUpdateDescriptorSets
    //   MaterialRegistry : buffer_->write(id.index, entry.data)
    //   MeshRegistry     : 何もしない（既定実装のまま）
    virtual void on_registered(Handle /*id*/, const Entry& /*entry*/) {}

    // destroy() から呼ばれる。entry の所有権が渡る。
    //   MeshRegistry     : shared_ptr へ載せ替えて DeletionQueue へ積む
    //   TextureRegistry  : bindless を既定テクスチャで埋め直す → by_path_ から消す → DeletionQueue へ
    //   MaterialRegistry : 何もしない（枠を空けるだけ）
    //
    // ★ 仮想関数をコンストラクタ／デストラクタから呼ばないこと（③-2）。
    //   on_destroy は派生の状態（deletions_ / by_path_ 等）を使うので、
    //   基底のデストラクタから呼ぶと派生部分は既に壊れている。
    //   基底のデストラクタは slots_ を**フックを通さずに**破棄するだけでよい
    //   （= 既存の各デストラクタの slots_.clear() と同じ挙動。DeletionQueue を通さないのは、
    //     Renderer が vkDeviceWaitIdle → flush_all() を先に済ませている前提だから）。
    virtual void on_destroy(Handle id, Entry&& entry) = 0;

private:
    // 1スロット = 「1つの実体を置ける枠」。解放しても枠自体は消さず、
    // generation を上げて再利用する（ecs::Registry の EntityRecord と同じ設計。phase14 D-5）。
    //
    // ★ std::vector<Slot> の再確保でムーブが走るので、Slot（= Entry）はムーブ可能であること。
    //   slots_.resize(n) してから代入する書き方は、型によってはコピーを要求するので避ける。
    struct Slot {
        Entry entry{};
        std::uint32_t generation = 0;
        bool alive = false;
        // ★ 手順8（③-4）で std::uint32_t ref_count = 0; を足す。
    };

    std::vector<Slot> slots_;                  // 添字が Handle::index
    std::vector<std::uint32_t> free_indices_;  // 解放済みスロットの再利用リスト
    std::uint32_t capacity_ = 0;               // 0 なら無制限
};

// ---------------------------------------------------------------------------
// メンバ関数の定義（ここから下はユーザーが書く）
//   contains / get / alive_count / コンストラクタ / full / emplace / destroy / get_mutable
// ---------------------------------------------------------------------------

}  // namespace sq::graphics
