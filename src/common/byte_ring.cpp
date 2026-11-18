#include "common/byte_ring.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mcast {

ByteRing::ByteRing(size_t capacity, size_t drop_granularity)
    : buffer_(std::max<size_t>(capacity, 4096)),
      capacity_(buffer_.size()),
      drop_granularity_(std::max<size_t>(drop_granularity, 1)) {}

void ByteRing::PushUnlocked(const uint8_t* data, size_t len) {
    if (len == 0) return;

    // Make room by dropping the oldest bytes, rounded down to the granularity
    // so TS packet boundaries stay intact.
    if (len >= capacity_) {
        const size_t keep = capacity_ - (capacity_ % drop_granularity_);
        const uint8_t* src = data + (len - keep);
        std::memcpy(buffer_.data(), src, keep);
        dropped_.fetch_add(size_ + (len - keep), std::memory_order_relaxed);
        head_ = 0;
        size_ = keep;
        return;
    }

    size_t free_space = capacity_ - size_;
    if (len > free_space) {
        // Drop at least the shortfall, rounded *up* to the protocol granularity.
        // Rounding down (the old behaviour) left less free space than requested
        // and let size_ grow past capacity_ under sustained overflow.
        const size_t need = len - free_space;
        size_t drop =
            ((need + drop_granularity_ - 1) / drop_granularity_) * drop_granularity_;
        if (drop > size_) drop = size_;
        if (drop == 0) drop = 1;
        head_ = (head_ + drop) % capacity_;
        size_ -= drop;
        dropped_.fetch_add(drop, std::memory_order_relaxed);
    }

    const size_t tail = (head_ + size_) % capacity_;
    const size_t first = std::min(len, capacity_ - tail);
    std::memcpy(buffer_.data() + tail, data, first);
    if (first < len) {
        std::memcpy(buffer_.data(), data + first, len - first);
    }
    size_ += len;
}

size_t ByteRing::Write(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return 0;
    PushUnlocked(data, len);
    if (size_ > 0) not_empty_.notify_one();
    return len;
}

size_t ByteRing::Read(uint8_t* dst, size_t cap, int timeout_ms) {
    if (!dst || cap == 0) return 0;

    std::unique_lock<std::mutex> lock(mutex_);
    if (size_ == 0 && !closed_) {
        not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return size_ > 0 || closed_; });
    }
    if (size_ == 0) return 0;

    const size_t n = std::min(cap, size_);
    const size_t first = std::min(n, capacity_ - head_);
    std::memcpy(dst, buffer_.data() + head_, first);
    if (first < n) {
        std::memcpy(dst + first, buffer_.data(), n - first);
    }

    head_ = (head_ + n) % capacity_;
    size_ -= n;
    not_full_.notify_one();
    return n;
}

size_t ByteRing::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void ByteRing::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

bool ByteRing::Closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

}  // namespace mcast
