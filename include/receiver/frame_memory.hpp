#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace receiver {

class FrameMemoryResource {
    std::mutex mutex_;
    std::atomic<bool> failed_{false};

  protected:
    // An unavailable resource returns nullptr. Ordinary heap exhaustion can throw.
    virtual uint8_t* try_allocate(size_t bytes) = 0;

  public:
    virtual ~FrameMemoryResource() = default;
    virtual void deallocate(uint8_t* data, size_t bytes) noexcept = 0;
    virtual bool cuda_pinned() const noexcept { return false; }

    bool failed() const noexcept { return failed_.load(); }
    uint8_t* allocate(size_t bytes) {
        std::lock_guard lock(mutex_);
        if (!failed_) {
            if (auto* data = try_allocate(bytes))
                return data;
            failed_ = true;
        }
        throw std::bad_alloc();
    }
};

class HeapFrameMemory final : public FrameMemoryResource {
    uint8_t* try_allocate(size_t bytes) override {
        return std::allocator<uint8_t>{}.allocate(bytes);
    }
    void deallocate(uint8_t* data, size_t bytes) noexcept override {
        std::allocator<uint8_t>{}.deallocate(data, bytes);
    }
};

inline const std::shared_ptr<FrameMemoryResource>& heap_frame_memory() {
    static const std::shared_ptr<FrameMemoryResource> resource = std::make_shared<HeapFrameMemory>();
    return resource;
}

template <class T> class FrameAllocator {
    template <class> friend class FrameAllocator;
    std::shared_ptr<FrameMemoryResource> memory_;

  public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    explicit FrameAllocator(std::shared_ptr<FrameMemoryResource> memory = {})
        : memory_(memory ? std::move(memory) : heap_frame_memory()) {}
    FrameAllocator(const FrameAllocator&) noexcept = default;
    FrameAllocator& operator=(const FrameAllocator&) noexcept = default;
    template <class U>
    FrameAllocator(const FrameAllocator<U>& other) noexcept : memory_(other.memory_) {}

    T* allocate(size_t count) {
        if constexpr (std::is_same_v<T, uint8_t>)
            return memory_->allocate(count);
        else
            return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data, size_t count) noexcept {
        if constexpr (std::is_same_v<T, uint8_t>)
            memory_->deallocate(data, count);
        else
            std::allocator<T>{}.deallocate(data, count);
    }
    // Independent copies use the heap. Pool entries choose their resource explicitly.
    FrameAllocator select_on_container_copy_construction() const { return FrameAllocator(); }
    template <class U> bool operator==(const FrameAllocator<U>& other) const noexcept {
        return memory_ == other.memory_;
    }
    const std::shared_ptr<FrameMemoryResource>& resource() const noexcept { return memory_; }
};

} // namespace receiver
