#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace od {

class FramePool;

/// Byte storage for one captured frame.
///
/// A frame at the receiver's native resolution is tens of megabytes, so
/// allocating one per frame costs an allocation, a zero-fill the caller
/// immediately overwrites, and a round of page faults. Buffers obtained from a
/// FramePool return to it when they die, which removes all three from the
/// steady-state capture path. A default-constructed buffer owns its storage
/// outright, so test and non-pooled code needs no pool.
class FrameBuffer {
public:
    FrameBuffer() = default;
    ~FrameBuffer();
    FrameBuffer(FrameBuffer&& other) noexcept;
    FrameBuffer& operator=(FrameBuffer&& other) noexcept;
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    [[nodiscard]] char* data() { return storage_.get(); }
    [[nodiscard]] const char* data() const { return storage_.get(); }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] std::string_view view() const { return {storage_.get(), size_}; }
    operator std::string_view() const { return view(); }  // NOLINT(*-explicit-*)

    /// Makes room for `count` bytes and leaves them uninitialized. Callers fill
    /// the whole buffer; nothing reads it before they do.
    void resizeUninitialized(std::size_t count);

    void assign(std::size_t count, char value);

    template <typename Iterator>
    void assign(Iterator first, Iterator last) {
        resizeUninitialized(static_cast<std::size_t>(std::distance(first, last)));
        std::copy(first, last, storage_.get());
    }

private:
    friend class FramePool;
    FrameBuffer(std::shared_ptr<FramePool> pool, std::unique_ptr<char[]> storage,
                std::size_t capacity, std::size_t size);
    void recycle();

    std::shared_ptr<FramePool> pool_;
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

/// Keeps a small set of frame-sized allocations alive between frames.
///
/// The pipeline holds at most a few frames at once -- one being filled, one
/// waiting, one being written to the encoder -- so a handful of retained
/// buffers covers the steady state. Acquiring never blocks and never fails: an
/// exhausted pool simply allocates, which keeps the realtime PipeWire callback
/// off any slow path.
class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    static constexpr std::size_t defaultRetained = 4;

    [[nodiscard]] static std::shared_ptr<FramePool> create(
        std::size_t retained = defaultRetained);

    [[nodiscard]] FrameBuffer acquire(std::size_t bytes);

    /// Buffers currently parked in the pool. Tests use this to observe reuse.
    [[nodiscard]] std::size_t retainedCount() const;

private:
    friend class FrameBuffer;
    explicit FramePool(const std::size_t retained) : retained_(retained) {}
    void give(std::unique_ptr<char[]> storage, std::size_t capacity);

    mutable std::mutex mutex_;
    std::vector<std::pair<std::unique_ptr<char[]>, std::size_t>> free_;
    std::size_t retained_;
};

}  // namespace od
