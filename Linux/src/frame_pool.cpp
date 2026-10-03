#include "opendisplay/frame_pool.hpp"

#include <cstring>

namespace od {

FrameBuffer::FrameBuffer(std::shared_ptr<FramePool> pool, std::unique_ptr<char[]> storage,
                         const std::size_t capacity, const std::size_t size)
    : pool_(std::move(pool)), storage_(std::move(storage)), capacity_(capacity), size_(size) {}

FrameBuffer::~FrameBuffer() { recycle(); }

FrameBuffer::FrameBuffer(FrameBuffer&& other) noexcept
    : pool_(std::move(other.pool_)), storage_(std::move(other.storage_)),
      capacity_(other.capacity_), size_(other.size_) {
    other.capacity_ = 0;
    other.size_ = 0;
}

FrameBuffer& FrameBuffer::operator=(FrameBuffer&& other) noexcept {
    if (this != &other) {
        recycle();
        pool_ = std::move(other.pool_);
        storage_ = std::move(other.storage_);
        capacity_ = other.capacity_;
        size_ = other.size_;
        other.capacity_ = 0;
        other.size_ = 0;
    }
    return *this;
}

void FrameBuffer::recycle() {
    if (pool_ && storage_ && capacity_ > 0) {
        pool_->give(std::move(storage_), capacity_);
    }
    pool_.reset();
    storage_.reset();
    capacity_ = 0;
    size_ = 0;
}

void FrameBuffer::resizeUninitialized(const std::size_t count) {
    if (count > capacity_) {
        // Growing abandons the old storage rather than returning it: the only
        // way to get here is a resolution change, after which the old size is
        // not wanted again.
        storage_ = std::make_unique_for_overwrite<char[]>(count);
        capacity_ = count;
    }
    size_ = count;
}

void FrameBuffer::assign(const std::size_t count, const char value) {
    resizeUninitialized(count);
    if (count > 0) {
        std::memset(storage_.get(), static_cast<unsigned char>(value), count);
    }
}

std::shared_ptr<FramePool> FramePool::create(const std::size_t retained) {
    return std::shared_ptr<FramePool>(new FramePool(retained));
}

FrameBuffer FramePool::acquire(const std::size_t bytes) {
    {
        std::lock_guard lock(mutex_);
        for (auto entry = free_.begin(); entry != free_.end(); ++entry) {
            if (entry->second >= bytes) {
                auto storage = std::move(entry->first);
                const auto capacity = entry->second;
                free_.erase(entry);
                return FrameBuffer(shared_from_this(), std::move(storage), capacity, bytes);
            }
        }
        // Nothing retained is big enough, so the frame size grew. Anything
        // still held is now dead weight.
        free_.clear();
    }
    return FrameBuffer(shared_from_this(), std::make_unique_for_overwrite<char[]>(bytes),
                       bytes, bytes);
}

void FramePool::give(std::unique_ptr<char[]> storage, const std::size_t capacity) {
    std::lock_guard lock(mutex_);
    if (free_.size() >= retained_) {
        return;
    }
    free_.emplace_back(std::move(storage), capacity);
}

std::size_t FramePool::retainedCount() const {
    std::lock_guard lock(mutex_);
    return free_.size();
}

}  // namespace od
