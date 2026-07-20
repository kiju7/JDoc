#pragma once
// member_pipeline.h - Opt-in conversion pipeline for archive walks
// (ArchiveLimits::threads > 1). The walk itself — decode, cap accounting,
// nested recursion — stays on the calling thread; only leaf-document
// conversion fans out to worker threads. Results are delivered through the
// user callback on the calling thread, preserving walk order.
// License: MIT

#include "convert_internal.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace jdoc {

class MemberPipeline {
public:
    // workers is the conversion thread count (>= 1 expected; the caller
    // resolves 0/auto before constructing). cb must outlive the pipeline.
    MemberPipeline(unsigned workers, const MemberCallback& cb);
    ~MemberPipeline();

    MemberPipeline(const MemberPipeline&) = delete;
    MemberPipeline& operator=(const MemberPipeline&) = delete;

    // Deliver an already-final result (errors, unsupported members, ...)
    // in walk order. Returns false once the walk should stop (the user
    // callback returned false earlier or now).
    bool emit(MemberResult&& r);

    // Queue a leaf document for conversion on a worker. partial carries
    // member_path/format/uncompressed_size; markdown (or error) is filled
    // in by the worker. Blocks when too many members are in flight, so
    // resident memory stays bounded by ~2×workers members.
    bool submit(MemberResult&& partial, FileFormat fmt,
                std::vector<char>&& data, const ConvertOptions& opts);

    // Wait for every queued conversion and deliver the tail in order.
    // Returns false if the walk was stopped early.
    bool finish();

private:
    struct Slot {
        MemberResult result;
        FileFormat fmt = FileFormat::UNKNOWN;
        std::vector<char> data;
        ConvertOptions opts;
        bool needs_convert = false;
        bool done = true;
    };

    void worker_loop();
    // Deliver every completed head slot to the user callback. Called with
    // the lock held; the callback itself runs unlocked.
    void deliver_ready(std::unique_lock<std::mutex>& lock);

    const MemberCallback& cb_;
    std::mutex mu_;
    std::condition_variable work_cv_;  // workers wait for queued tasks
    std::condition_variable done_cv_;  // walker waits for the head to finish
    std::deque<std::unique_ptr<Slot>> slots_;  // delivery order
    std::deque<Slot*> queue_;                  // undispatched conversions
    std::vector<std::thread> workers_;
    size_t inflight_limit_;
    size_t inflight_ = 0;  // undelivered slots holding member data
    bool stopped_ = false;
    bool shutdown_ = false;
};

} // namespace jdoc
