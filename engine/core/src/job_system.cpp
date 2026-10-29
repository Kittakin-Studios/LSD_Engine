#include "lsd/core/job_system.h"
#include "lsd/foundation/assert.h"

#include <new>

namespace lsd {

namespace {
u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}
} // namespace

JobSystem::JobSystem(Allocator& allocator, u32 worker_count, u32 queue_capacity)
    : allocator_(allocator), worker_count_(worker_count) {
    const u32 capacity = next_pow2(queue_capacity < 2 ? 2 : queue_capacity);
    capacity_mask_ = capacity - 1;
    queue_ = static_cast<Job*>(allocator_.allocate(sizeof(Job) * capacity, alignof(Job)));
    if (worker_count_ > 0) {
        workers_ = static_cast<std::thread*>(allocator_.allocate(sizeof(std::thread) * worker_count_, alignof(std::thread)));
        for (u32 i = 0; i < worker_count_; ++i) new (&workers_[i]) std::thread([this] { worker_main(); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    has_work_.notify_all();
    for (u32 i = 0; i < worker_count_; ++i) {
        workers_[i].join();
        workers_[i].~thread();
    }
    if (workers_) allocator_.deallocate(workers_);
    allocator_.deallocate(queue_);
}

void JobSystem::run(JobFn fn, void* data, JobCounter* counter) {
    LSD_ASSERT(fn != nullptr);
    if (counter) counter->pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock lock(mutex_);
        // Full queue: wait for a worker to free a slot. With no workers, run inline instead.
        while (tail_ - head_ > capacity_mask_) {
            if (worker_count_ == 0) {
                Job job;
                try_pop(job);
                lock.unlock();
                execute(job);
                lock.lock();
                continue;
            }
            has_space_.wait(lock);
        }
        queue_[tail_ & capacity_mask_] = Job{fn, data, counter};
        ++tail_;
    }
    has_work_.notify_one();
}

void JobSystem::wait(JobCounter& counter) {
    while (counter.pending.load(std::memory_order_acquire) != 0) {
        Job job;
        bool got = false;
        {
            std::lock_guard lock(mutex_);
            got = try_pop(job);
        }
        if (got) {
            has_space_.notify_one();
            execute(job);
        } else {
            std::this_thread::yield();  // our jobs are running on workers; spin politely
        }
    }
}

bool JobSystem::try_pop(Job& out) {
    if (head_ == tail_) return false;
    out = queue_[head_ & capacity_mask_];
    ++head_;
    return true;
}

void JobSystem::execute(const Job& job) {
    job.fn(job.data);
    if (job.counter) job.counter->pending.fetch_sub(1, std::memory_order_release);
}

void JobSystem::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            has_work_.wait(lock, [this] { return stopping_ || head_ != tail_; });
            if (!try_pop(job)) return;  // stopping and queue drained
        }
        has_space_.notify_one();
        execute(job);
    }
}

} // namespace lsd
