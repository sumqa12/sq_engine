#include "sq/graphics/deletion_queue.hpp"

#include <spdlog/spdlog.h>

namespace sq::graphics {

DeletionQueue::DeletionQueue(const std::uint32_t frames_to_wait)
    : frames_to_wait_(frames_to_wait) {
}

DeletionQueue::~DeletionQueue() {
    // pending_ が空でなければ spdlog::warn を出す。
    //   ここに残っているということは、Renderer が flush_all() を呼ばずに壊したということ。
    //   この時点では VkDevice が既に無い可能性があり、破棄を実行するのはかえって危険。
    //   「黙って漏らす」より「気付ける形で漏らす」方がよい。
    if (!pending_.empty()) {
        spdlog::warn("DeletionQueue::~DeletionQueue : 破棄されていないものが残っています。 個数 : {}", pending_.size());
    }
}

void DeletionQueue::push(std::function<void()> destroyer) {
    pending_.push_back(Pending{ std::move(destroyer), frames_to_wait_ });
}

void DeletionQueue::flush_expired() {
    //   全件の remaining を1減らし、0 になったものを実行して取り除く。
    //   std::erase_if で「実行しつつ取り除く」のが簡潔
    std::erase_if(pending_, [](Pending& p) {
        if (p.remaining > 0) { --p.remaining; return false; }
        p.destroyer();
        return true;
    });
    //   ★ erase_if の述語は要素を複数回評価しない保証がある（副作用を入れてよい）。
    //     気持ち悪ければ「実行対象を別 vector へ move してから消す」2段構えでもよい。
}

void DeletionQueue::flush_all() {
    //   ★ 呼び出し側が vkDeviceWaitIdle を済ませている前提。
    for (auto&[destroyer, _] : pending_) {
        destroyer();
    }

    pending_.clear();
}

}  // namespace sq::graphics
