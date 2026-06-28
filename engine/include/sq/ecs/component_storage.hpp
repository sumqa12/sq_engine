#pragma once

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace sq::ecs {

// アーキタイプ内の単一のコンポーネント型に対する、型が削除された高密度なコンポーネント配列。
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;

    [[nodiscard]] virtual std::size_t size() const = 0;

    // `index` の位置にある要素を、最後の要素と入れ替えてポップすることで削除します。
    // (O(1) ですが、最後のスロットに格納されていたインデックスの以前の所有者の有効性が失われます)。
    virtual void swap_remove(std::size_t index) = 0;

    // このストレージの `src_index` にある要素を `dest` に移動し（同じ具体的なコンポーネント型でなければならない）、
    // その後 swap_remove を使用してこのストレージからその要素を削除します。
    virtual void move_element_to(std::size_t src_index, IComponentStorage& dest) = 0;

    // これと同じ具体的なコンポーネント型を持つ、新しい空のストレージを作成します。
    // アーキタイプ遷移において、まだ具体的な C++ 型が定義されていないコンポーネント型（レジストリは型 ID しか認識していない）に対して、新しいストレージが必要な場合に使用されます。
    [[nodiscard]] virtual std::unique_ptr<IComponentStorage> create_empty() const = 0;
};

template <typename T>
class ComponentStorage final : public IComponentStorage {
public:
    [[nodiscard]] std::size_t size() const override { return data_.size(); }

    template <typename... Args>
    T& emplace(Args&&... args) {
        return data_.emplace_back(std::forward<Args>(args)...);
    }

    T& get(std::size_t index) {
        assert(index < data_.size());
        return data_[index];
    }

    const T& get(std::size_t index) const {
        assert(index < data_.size());
        return data_[index];
    }

    void swap_remove(std::size_t index) override {
        assert(index < data_.size());
        if (index != data_.size() - 1) {
            data_[index] = std::move(data_.back());
        }
        data_.pop_back();
    }

    void move_element_to(std::size_t src_index, IComponentStorage& dest) override {
        assert(index_in_range(src_index));
        auto& typed_dest = static_cast<ComponentStorage<T>&>(dest);
        typed_dest.emplace(std::move(data_[src_index]));
        swap_remove(src_index);
    }

    [[nodiscard]] std::unique_ptr<IComponentStorage> create_empty() const override {
        return std::make_unique<ComponentStorage<T>>();
    }

private:
    [[nodiscard]] bool index_in_range(std::size_t index) const { return index < data_.size(); }

    std::vector<T> data_;
};

}  // namespace sq::ecs
