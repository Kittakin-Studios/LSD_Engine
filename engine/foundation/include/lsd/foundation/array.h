#pragma once
// Growable array for plain-data elements, backed by an Allocator. Elements are moved with memcpy,
// so T must be trivially copyable (no constructors/destructors run).
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/assert.h"

#include <cstring>
#include <type_traits>

namespace lsd {

template <typename T>
class Array {
    static_assert(std::is_trivially_copyable_v<T>, "Array<T> holds plain data only");

public:
    explicit Array(Allocator& allocator) : allocator_(&allocator) {}
    ~Array() { if (data_) allocator_->deallocate(data_); }
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    void reserve(usize capacity) {
        if (capacity <= capacity_) return;
        T* grown = static_cast<T*>(allocator_->allocate(sizeof(T) * capacity, alignof(T)));
        if (size_) std::memcpy(grown, data_, sizeof(T) * size_);
        if (data_) allocator_->deallocate(data_);
        data_ = grown;
        capacity_ = capacity;
    }
    void resize(usize size) {
        // Grows by at least doubling, like push_back: appending in many small resizes stays linear
        // (growing to the exact size made saving a 3000-object level take 6.5 s instead of milliseconds).
        if (size > capacity_) reserve(size > capacity_ * 2 ? size : capacity_ * 2);
        size_ = size;  // new elements are uninitialised
    }
    void push_back(const T& value) {
        if (size_ == capacity_) reserve(capacity_ ? capacity_ * 2 : 16);
        data_[size_++] = value;
    }
    void clear() { size_ = 0; }
    // Removes element i by moving the last element into its place (order not kept).
    void remove_swap(usize i) {
        LSD_ASSERT(i < size_);
        data_[i] = data_[--size_];
    }

    T& operator[](usize i) { LSD_ASSERT(i < size_); return data_[i]; }
    const T& operator[](usize i) const { LSD_ASSERT(i < size_); return data_[i]; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    usize size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Allocator& allocator() const { return *allocator_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

private:
    Allocator* allocator_;
    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

} // namespace lsd
