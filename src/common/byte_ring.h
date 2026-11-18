#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mcast {

// Single-producer / single-consumer byte ring buffer.
//
// The producer (FFmpeg muxer write callback) must never block, otherwise the
// demux/mux pipeline stalls. When the buffer is full we therefore drop the
// oldest bytes and count them, which keeps latency bounded and makes the loss
// visible in the stats instead of hidden in the kernel socket buffer.
class ByteRing {
public:
    // drop_granularity keeps the drop point aligned to a protocol unit
    // (188 bytes for MPEG-TS) so a lossy buffer cannot desynchronise the stream.
    explicit ByteRing(size_t capacity, size_t drop_granularity = 1);

    // Non-blocking. Always accepts the write; returns how many bytes were kept.
    size_t Write(const uint8_t* data, size_t len);

    // Blocks until data is available, the ring is closed, or timeout expires.
    // Returns 0 on timeout/close.
    size_t Read(uint8_t* dst, size_t cap, int timeout_ms);

    size_t Size() const;
    size_t Capacity() const { return capacity_; }
    uint64_t DroppedBytes() const { return dropped_.load(std::memory_order_relaxed); }

    void Close();
    bool Closed() const;

private:
    void PushUnlocked(const uint8_t* data, size_t len);

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::vector<uint8_t> buffer_;
    size_t capacity_;
    size_t drop_granularity_;
    size_t head_ = 0;  // read position
    size_t size_ = 0;
    bool closed_ = false;
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace mcast
