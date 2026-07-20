// member_pipeline.cpp - Ordered conversion pipeline for archive walks
// License: MIT

#include "archive/member_pipeline.h"

namespace jdoc {

MemberPipeline::MemberPipeline(unsigned workers, const MemberCallback& cb)
    : cb_(cb), inflight_limit_(static_cast<size_t>(workers) * 2 + 2) {
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; i++)
        workers_.emplace_back(&MemberPipeline::worker_loop, this);
}

MemberPipeline::~MemberPipeline() {
    {
        std::unique_lock<std::mutex> lock(mu_);
        stopped_ = true;  // abandon undelivered results on an abnormal exit
        shutdown_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void MemberPipeline::worker_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        work_cv_.wait(lock, [&] { return shutdown_ || !queue_.empty(); });
        if (queue_.empty()) return;  // shutdown with nothing left to do
        Slot* slot = queue_.front();
        queue_.pop_front();
        if (stopped_) {  // results are being discarded — skip the work
            slot->data = std::vector<char>();
            slot->done = true;
            done_cv_.notify_all();
            continue;
        }
        lock.unlock();

        try {
            slot->result.markdown = convert_from_memory_as(
                slot->fmt, reinterpret_cast<const uint8_t*>(slot->data.data()),
                slot->data.size(), slot->result.member_path, slot->opts);
        } catch (const std::exception& e) {
            slot->result.error_code = MemberErrorCode::CONVERT_FAILED;
            slot->result.error = e.what();
        } catch (...) {
            slot->result.error_code = MemberErrorCode::CONVERT_FAILED;
            slot->result.error = "conversion failed";
        }
        slot->data = std::vector<char>();

        lock.lock();
        slot->done = true;
        done_cv_.notify_all();
    }
}

void MemberPipeline::deliver_ready(std::unique_lock<std::mutex>& lock) {
    while (!stopped_ && !slots_.empty() && slots_.front()->done) {
        std::unique_ptr<Slot> slot = std::move(slots_.front());
        slots_.pop_front();
        if (slot->needs_convert && inflight_ > 0) inflight_--;
        lock.unlock();
        bool keep_going = cb_(std::move(slot->result));
        lock.lock();
        if (!keep_going) stopped_ = true;
    }
}

bool MemberPipeline::emit(MemberResult&& r) {
    std::unique_lock<std::mutex> lock(mu_);
    if (stopped_) return false;
    auto slot = std::make_unique<Slot>();
    slot->result = std::move(r);
    slots_.push_back(std::move(slot));
    deliver_ready(lock);
    return !stopped_;
}

bool MemberPipeline::submit(MemberResult&& partial, FileFormat fmt,
                            std::vector<char>&& data,
                            const ConvertOptions& opts) {
    std::unique_lock<std::mutex> lock(mu_);
    if (stopped_) return false;
    auto slot = std::make_unique<Slot>();
    slot->result = std::move(partial);
    slot->fmt = fmt;
    slot->data = std::move(data);
    slot->opts = opts;
    slot->needs_convert = true;
    slot->done = false;
    queue_.push_back(slot.get());
    slots_.push_back(std::move(slot));
    inflight_++;
    work_cv_.notify_one();

    // Backpressure: bound the number of member buffers alive at once.
    while (!stopped_ && inflight_ >= inflight_limit_) {
        deliver_ready(lock);
        if (stopped_ || inflight_ < inflight_limit_) break;
        done_cv_.wait(lock, [&] {
            return stopped_ || (!slots_.empty() && slots_.front()->done);
        });
    }
    deliver_ready(lock);
    return !stopped_;
}

bool MemberPipeline::finish() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stopped_ && !slots_.empty()) {
        done_cv_.wait(lock, [&] {
            return stopped_ || (!slots_.empty() && slots_.front()->done);
        });
        deliver_ready(lock);
    }
    shutdown_ = true;
    work_cv_.notify_all();
    return !stopped_;
}

} // namespace jdoc
