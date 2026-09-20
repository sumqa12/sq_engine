#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace sq::graphics {

// GPUリソースの遅延解放キュー（phase14 ② / D-5）。
//
// なぜ必要か:
//   unload() を呼んだ時点では、そのリソースを参照するコマンドバッファが
//   まだ GPU で実行中かもしれない（kFramesInFlight 枚が飛んでいる）。
//   即座に vkDestroy* すると「使用中のリソースを破棄した」でバリデーションが叫ぶか、
//   環境によっては黙って壊れる（しかも再現性が低く、原因の特定が非常に難しい）。
//
// 対策:
//   「今から frames_to_wait フレーム後に破棄する」キューに積み、
//   フレーム先頭で期限の来たものだけ実際に破棄する。
//   kFramesInFlight フレーム待てば、unload 時に飛んでいたコマンドバッファは
//   すべて完了している（フェンス待機を経ているため）。
//
// ★ Renderer が1つ持ち、draw_frame の先頭（フェンス待機の**直後**）で flush_expired() を呼ぶ。
//   フェンス待機より前に呼ぶと、カウンタだけ進んで実際には完了していないフレームを
//   「終わった」と数えてしまう。
// ★ デストラクタでは vkDeviceWaitIdle → flush_all() の順。
class DeletionQueue {
public:
    explicit DeletionQueue(std::uint32_t frames_to_wait);
    ~DeletionQueue();  // ★ 残っているものを破棄するが、device が生きている保証は無い。
                       //   Renderer が明示的に flush_all() してから壊すこと。

    DeletionQueue(const DeletionQueue&) = delete;
    DeletionQueue& operator=(const DeletionQueue&) = delete;

    // 破棄処理を frames_to_wait フレーム後に実行するよう予約する。
    //
    // ★ 詰まりどころ: std::function はコピー構築可能な呼び出し可能物しか格納できない。
    //   unique_ptr をムーブキャプチャしたラムダは copy-constructible ではないので
    //   **そのままでは入らない**（エラーメッセージも難解）。対処は3択:
    //     a) shared_ptr にして値キャプチャする（最も簡単）
    //     b) std::move_only_function（C++23）を使う ← このプロジェクトは C++20 なので不可
    //     c) 専用の型消去（move-only な関数ラッパ）を自前で書く ← 学習的にはこれが一番得
    void push(std::function<void()> destroyer);

    // フレーム先頭で1回呼ぶ。全件の remaining を1減らし、0 になったものを実行して取り除く。
    // ★ 実行中に push が増えることは無い前提（破棄処理の中で unload を呼ばないこと）。
    void flush_expired();

    // 残り全件を remaining に関わらず即実行する。
    // ★ 必ず vkDeviceWaitIdle の**後**に呼ぶこと（待たずに呼べば遅延の意味が消える）。
    void flush_all();

private:
    struct Pending {
        std::function<void()> destroyer;
        std::uint32_t remaining = 0;  // あと何フレーム待つか
    };

    std::vector<Pending> pending_;
    std::uint32_t frames_to_wait_ = 0;
};

}  // namespace sq::graphics
