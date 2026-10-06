#pragma once

namespace sq::graphics {

// アセットへの所有参照（phase16 手順8 / ③-4 / D-9）。
//
// コピーすると +1、破棄すると -1。0 になった時点でレジストリが実体を解放する。
// shared_ptr との違いは「カウントがレジストリのスロット側にある」こと
// （ハンドルは 8 バイトのままで、制御ブロックを別途確保しない）。
//
// Registry: MeshRegistry / TextureRegistry / MaterialRegistry
//           （retain(Handle) / release(Handle) / contains(Handle) を持つもの）
// Handle:   scene::MeshId / TextureId / MaterialId
//
// ★ 置き場所を plan の scene/ から graphics/ へ変えた。
//   scene（ECS コンポーネント側）は graphics を知らない層で、コンポーネントは
//   生ハンドルのまま（D-9）なので、scene が AssetRef を必要とする場面が無い。
//   scene に置くと「scene → graphics」の逆向きの依存が1本増える。
//
// ★ 別名（MeshRef / TextureRef / MaterialRef）は各レジストリのヘッダの末尾で定義する。
//   ここで前方宣言して別名を作ることもできるが、別名を使う側は結局
//   レジストリの完全な定義を要する（retain / release を呼ぶため）ので、
//   レジストリのヘッダに置く方が include が1本で済む。
//
// ★ テンプレートなのでヘッダオンリー。
template <typename Registry, typename Handle>
class AssetRef {
public:
    // 空の参照（何も所有しない）。valid() == false。
    // ★ MaterialEntry / LoadedModel の既定構築に要る。
    AssetRef() = default;

    // id を所有する。ここで registry.retain(id) を呼ぶ（0 → 1）。
    //
    // ★ registry.contains(id) でなければ**空の参照になる**（retain もしない）。
    //   kInvalidTextureId（マテリアルのテクスチャ無しスロット）を包むのは正常な使い方なので、
    //   例外にはしないこと。
    // ★ retain するのは**このコンストラクタとコピーだけ**。
    //   レジストリの add() / load() は生の Handle を返すだけで retain しない
    //   （load のキャッシュヒットも同じ。ここを2箇所にすると二重に数える。③-4）。
    AssetRef(Registry& registry, Handle id) {
        if (registry.contains(id)) {
            registry.retain(id);
            registry_ = &registry;
            id_ = id;
        }
    }

    // 同じものをもう1本所有する（other が有効なら retain）。
    AssetRef(const AssetRef& other) {
        if (other.valid()) {
            other.registry_->retain(other.id_);
        }
        registry_ = other.registry_;
        id_ = other.id_;
    }

    // 所有権の移譲。retain しない。other を空にする（registry_ = nullptr, id_ = {}）。
    // ★ noexcept を付けること。std::vector<AssetRef> の再確保がコピーではなく
    //   ムーブを選ぶのは noexcept のときだけ（付けないと再確保のたびに retain / release が走る）。
    AssetRef(AssetRef&& other) noexcept {
        registry_ = std::exchange(other.registry_, nullptr);
        id_ = std::exchange(other.id_, {});
    }

    // 所有していれば release。
    ~AssetRef() {
        reset();
    }

    // ★ 自己代入を必ず扱うこと。a = a; で release → retain の順に走ると、
    //   カウント 1 の状態で解放されて**ダングリングになる**。
    //   「先に other を retain してから自分を release する」順にすれば、
    //   自己代入も「同じアセットへの付け替え」も同じコードで安全になる。
    AssetRef& operator=(const AssetRef& other) {
        if (this == &other) { return *this; }

        if (other.valid()) {
            other.registry_->retain(other.id_);
        }

        reset();
        registry_ = other.registry_;
        id_ = other.id_;
        return *this;
    }

    // ★ 自己ムーブ代入（a = std::move(a)）でも壊れないこと。
    //   this == &other なら何もしない、で足りる。
    AssetRef& operator=(AssetRef&& other) noexcept {
        if (this == &other) { return *this; }

        reset();
        registry_ = std::exchange(other.registry_, nullptr);
        id_ = std::exchange(other.id_, {});
        return *this;
    }

    // GPU へ渡す生の添字はここから取る（id().index）。
    [[nodiscard]] Handle id() const {
        return id_;
    }

    // 何かを所有しているか。
    // ★ 「まだ生きているか」ではない。レジストリ側で unload()（強制解放）された後も true のまま。
    //   生死を見たいときは registry.contains(id()) を使うこと。
    [[nodiscard]] bool valid() const {
        return registry_ != nullptr;
    }

    // release して空にする。
    // ★ Renderer のデストラクタで既定アセットの参照を、レジストリより先に落とすのに使う（③-5）。
    void reset() {
        if (valid()) {
            registry_->release(id_);
            registry_ = nullptr;
            id_ = {};
        }
    }

private:
    Registry* registry_ = nullptr;   // 所有しない
    Handle id_{};
};

}  // namespace sq::graphics
