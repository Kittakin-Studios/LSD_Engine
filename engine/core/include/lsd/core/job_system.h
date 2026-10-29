#pragma once
// Job system: a fixed pool of worker threads pulling small jobs from a shared queue.
// Callers group jobs with a JobCounter and wait on it; a waiting thread runs queued jobs
// itself instead of sleeping, so waits never waste a core.
#include "lsd/foundation/allocator.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace lsd {

using JobFn = void (*)(void* data);

// Number of unfinished jobs in a group. Zero means everything in the group is done.
struct JobCounter {
    std::atomic<u32> pending{0};
};

class JobSystem {
public:
    // queue_capacity is the most jobs that can be waiting at once (rounded up to a power of two).
    JobSystem(Allocator& allocator, u32 worker_count, u32 queue_capacity = 4096);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Queues fn(data). If counter is set it is incremented now and decremented when the job ends.
    void run(JobFn fn, void* data, JobCounter* counter = nullptr);

    // Runs queued jobs on the calling thread until counter reaches zero.
    void wait(JobCounter& counter);

    u32 worker_count() const { return worker_count_; }

private:
    struct Job {
        JobFn fn;
        void* data;
        JobCounter* counter;
    };

    bool try_pop(Job& out);  // caller holds mutex_
    static void execute(const Job& job);
    void worker_main();

    Allocator& allocator_;
    Job* queue_ = nullptr;
    u32 capacity_mask_ = 0;
    u32 head_ = 0;  // next job to pop
    u32 tail_ = 0;  // next free slot
    std::mutex mutex_;
    std::condition_variable has_work_;
    std::condition_variable has_space_;
    bool stopping_ = false;
    std::thread* workers_ = nullptr;
    u32 worker_count_ = 0;
};

} // namespace lsd
