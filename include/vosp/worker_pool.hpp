#pragma once

/** @file worker_pool.hpp Bounded worker pool and task lifecycle policies. */

#include <vosp/error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(VOSP_ENABLE_HELGRIND_ANNOTATIONS)
#include <valgrind/helgrind.h>
#endif

namespace vosp::async
{
    using vosp::error::Category;
    using vosp::error::Error;
    using vosp::error::OperationResult;

    inline constexpr std::size_t max_worker_count = 1024;
    inline constexpr std::size_t max_queue_capacity = 1024;
    inline constexpr std::uint32_t task_cancelled_code = 0xE004;

    namespace detail
    {
        inline void helgrind_happens_before(const void* address) noexcept
        {
#if defined(VOSP_ENABLE_HELGRIND_ANNOTATIONS)
            ANNOTATE_HAPPENS_BEFORE(address);
#else
            static_cast<void>(address);
#endif
        }

        inline void helgrind_happens_after(const void* address) noexcept
        {
#if defined(VOSP_ENABLE_HELGRIND_ANNOTATIONS)
            ANNOTATE_HAPPENS_AFTER(address);
#else
            static_cast<void>(address);
#endif
        }
    }

    /** @brief Defines what happens to queued work during shutdown. */
    enum class ShutdownMode : std::uint8_t
    {
        DRAIN,
        CANCEL_PENDING
    };

    /** @brief Creates the stable error returned to futures cancelled in the queue. */
    [[nodiscard]] inline Error task_cancelled_error()
    {
        return Error{Category::NONE, task_cancelled_code,
                     "Worker pool task was cancelled before execution"};
    }

    /**
     * @brief Bounded, owning worker pool compatible with AsyncSystem.
     *
     * The pool has a hard limit of 1024 workers and 1024 queued tasks. Queue
     * slots are allocated once during construction and reused as a ring.
     * Submitting to a full queue applies blocking backpressure until a slot
     * is released or shutdown begins. Queued tasks can be cancelled and
     * their futures complete with task_cancelled_code. A task that already
     * started cannot be interrupted safely without cooperative cancellation.
     */
    class IndustrialWorkerPool final
    {
    public:
        using Task = std::function<OperationResult()>;
        using CancellableTask = std::function<OperationResult(std::stop_token)>;

        /**
         * @brief Creates a bounded worker pool.
         * @param worker_count Number of workers; zero selects hardware concurrency.
         * @param queue_capacity Maximum number of waiting tasks.
         * @throws std::invalid_argument For values outside the supported limits.
         */
        explicit IndustrialWorkerPool(
            std::size_t worker_count = 0,
            std::size_t queue_capacity = max_queue_capacity)
            : queue_capacity_{validate_queue_capacity(queue_capacity)},
              tasks_(queue_capacity_)
        {
            if (worker_count == 0)
            {
                worker_count = std::thread::hardware_concurrency();
            }

            if (worker_count == 0 || worker_count > max_worker_count)
            {
                throw std::invalid_argument("Worker pool worker count must be in [1, 1024]");
            }

            worker_count_ = worker_count;
            workers_.reserve(worker_count);
            try
            {
                for (std::size_t index = 0; index < worker_count; ++index)
                {
                    workers_.emplace_back([this](std::stop_token stop_token)
                    {
                        run(stop_token);
                    });
                }
            }
            catch (...)
            {
                queue_state_.fetch_or(stopping_mask_, std::memory_order_release);
                queue_state_.notify_all();
                workers_.clear();
                throw;
            }
        }

        IndustrialWorkerPool(const IndustrialWorkerPool&) = delete;
        IndustrialWorkerPool& operator=(const IndustrialWorkerPool&) = delete;

        /**
         * @brief Submits a task, blocking while the bounded queue is full.
         * @return Future containing the task result or a cancellation error.
         * @throws std::runtime_error If shutdown has started.
         */
        [[nodiscard]] std::future<OperationResult> submit(Task task)
        {
            TaskItem item{std::move(task), std::in_place};
            std::future<OperationResult> result = item.completion->get_future();
            enqueue(std::move(item));
            return result;
        }

        /**
         * @brief Submits a task that can cooperatively observe cancellation.
         * @param task Callback that must check stop_token and return promptly.
         * @return Future containing the task result.
         * @throws std::runtime_error If shutdown has started.
         */
        [[nodiscard]] std::future<OperationResult> submit_cancellable(CancellableTask task)
        {
            TaskItem item{std::move(task), std::in_place};
            std::future<OperationResult> result = item.completion->get_future();
            enqueue(std::move(item));
            return result;
        }

        /**
         * @brief Submits work without allocating a promise/future shared state.
         * @param task Callback whose result is accounted by failed_dispatches().
         * @throws std::runtime_error If shutdown has started.
         */
        void dispatch(Task task)
        {
            enqueue(TaskItem{std::move(task), std::nullopt});
        }

        /**
         * @brief Submits fire-and-forget work with cooperative cancellation.
         * @param task Callback that must check stop_token and return promptly.
         * @throws std::runtime_error If shutdown has started.
         */
        void dispatch_cancellable(CancellableTask task)
        {
            enqueue(TaskItem{std::move(task), std::nullopt});
        }

        /**
         * @brief Moves a batch of fire-and-forget tasks into the bounded queue.
         * @param tasks Callbacks consumed in order; accepted elements are moved from.
         * @return Number of callbacks accepted before completion or shutdown.
         * @note Uses grouped queue refills and claims up to 16 bulk callbacks per
         * worker. Claimed callbacks are active and no longer queue-cancellable.
         */
        [[nodiscard]] std::size_t dispatch_bulk(std::span<Task> tasks)
        {
            std::size_t accepted = 0;
            while (accepted < tasks.size())
            {
                std::size_t enqueued = 0;
                {
                    auto state = wait_for_space();
                    if (is_stopping_state(state))
                    {
                        return accepted;
                    }

                    const std::lock_guard producer_lock{producer_mutex_};
                    state = queue_state_.load(std::memory_order_acquire);
                    if (is_stopping_state(state)) return accepted;

                    const auto available = queue_capacity_ - pending_from_state(state);
                    if (available == 0) continue;

                    detail::helgrind_happens_after(&queue_state_);
                    enqueued = std::min(tasks.size() - accepted, available);
                    for (std::size_t index = 0; index < enqueued; ++index)
                    {
                        tasks_[queue_tail_].emplace(
                            TaskItem{std::move(tasks[accepted + index]), std::nullopt, true});
                        queue_tail_ = next_queue_index(queue_tail_);
                    }
                    accepted += enqueued;
                    detail::helgrind_happens_before(&queue_state_);
                    queue_state_.fetch_add(enqueued, std::memory_order_release);
                }

                if (waiting_workers_.load(std::memory_order_relaxed) != 0)
                {
                    if (enqueued == 1) queue_state_.notify_one();
                    else queue_state_.notify_all();
                }
            }

            return accepted;
        }

        /** @brief Returns failed or throwing fire-and-forget task count. */
        [[nodiscard]] std::size_t failed_dispatches() const noexcept
        {
            return failed_dispatches_.load(std::memory_order_relaxed);
        }

        /** @brief Returns fire-and-forget tasks removed before execution. */
        [[nodiscard]] std::size_t cancelled_dispatches() const noexcept
        {
            return cancelled_dispatches_.load(std::memory_order_relaxed);
        }

        /** @brief Blocks until no queued or executing tasks remain. */
        void wait()
        {
            while (!is_idle())
            {
                waiting_for_idle_.fetch_add(1, std::memory_order_relaxed);
                const auto epoch = completion_epoch_.load(std::memory_order_acquire);
                if (!is_idle())
                    completion_epoch_.wait(epoch, std::memory_order_acquire);
                waiting_for_idle_.fetch_sub(1, std::memory_order_relaxed);
            }
            detail::helgrind_happens_after(&queue_state_);
        }

    private:
        struct TaskItem
        {
            template<typename Callback, typename CompletionTag>
                requires std::same_as<std::remove_cvref_t<Callback>, Task> ||
                         std::same_as<std::remove_cvref_t<Callback>, CancellableTask>
            TaskItem(Callback&& value, CompletionTag completion_tag, bool bulk = false)
                : callback{std::in_place_type<std::remove_cvref_t<Callback>>,
                           std::forward<Callback>(value)},
                  completion{completion_tag},
                  bulk{bulk}
            {
            }

            std::variant<Task, CancellableTask> callback;
            std::optional<std::promise<OperationResult>> completion;
            bool bulk = false;
        };

        void enqueue(TaskItem item)
        {
            while (true)
            {
                auto state = wait_for_space();
                if (is_stopping_state(state))
                    throw std::runtime_error("Cannot submit task after worker pool shutdown");

                {
                    const std::lock_guard producer_lock{producer_mutex_};
                    state = queue_state_.load(std::memory_order_acquire);
                    if (is_stopping_state(state))
                        throw std::runtime_error("Cannot submit task after worker pool shutdown");
                    if (pending_from_state(state) == queue_capacity_) continue;

                    detail::helgrind_happens_after(&queue_state_);
                    tasks_[queue_tail_].emplace(std::move(item));
                    queue_tail_ = next_queue_index(queue_tail_);
                    detail::helgrind_happens_before(&queue_state_);
                    queue_state_.fetch_add(1, std::memory_order_release);
                }

                if (waiting_workers_.load(std::memory_order_relaxed) != 0)
                    queue_state_.notify_one();
                return;
            }
        }

        [[nodiscard]] std::size_t wait_for_space() noexcept
        {
            auto state = queue_state_.load(std::memory_order_acquire);
            while (!is_stopping_state(state) &&
                   pending_from_state(state) == queue_capacity_)
            {
                waiting_producers_.fetch_add(1, std::memory_order_relaxed);
                const auto observed = queue_state_.load(std::memory_order_acquire);
                if (!is_stopping_state(observed) &&
                    pending_from_state(observed) == queue_capacity_)
                    queue_state_.wait(observed, std::memory_order_acquire);
                waiting_producers_.fetch_sub(1, std::memory_order_relaxed);
                state = queue_state_.load(std::memory_order_acquire);
            }
            return state;
        }

    public:
        /** @brief Returns the fixed number of worker threads. */
        [[nodiscard]] std::size_t worker_count() const noexcept { return worker_count_; }

        /** @brief Returns the maximum number of queued tasks. */
        [[nodiscard]] std::size_t queue_capacity() const noexcept { return queue_capacity_; }

        /**
         * @brief Returns bytes reserved for the fixed ring queue slots.
         * @note Excludes the pool object, worker stacks, and heap allocations made
         * by callables that do not fit inside std::function's local storage.
         */
        [[nodiscard]] std::size_t queue_storage_bytes() const noexcept
        {
            return tasks_.capacity() * sizeof(tasks_.front());
        }

        /** @brief Returns the number of tasks waiting to start. */
        [[nodiscard]] std::size_t pending_tasks() const noexcept
        {
            return pending_from_state(queue_state_.load(std::memory_order_acquire));
        }

        /** @brief Returns whether the pool rejects new submissions. */
        [[nodiscard]] bool is_stopping() const noexcept
        {
            return is_stopping_state(queue_state_.load(std::memory_order_acquire));
        }

        /** @brief Returns tasks claimed for execution, including bulk chunks. */
        [[nodiscard]] std::size_t active_tasks() const noexcept
        {
            return active_from_state(queue_state_.load(std::memory_order_acquire));
        }

        /**
         * @brief Cancels all tasks that have not started.
         * @return Number of cancelled queued tasks.
         */
        [[nodiscard]] std::size_t clear_queue() noexcept
        {
            return cancel_pending();
        }

        /**
         * @brief Stops the pool and joins all workers.
         * @param mode Drain queued tasks or cancel them before joining.
         * @note DRAIN does not request cooperative cancellation. CANCEL_PENDING
         * requests cancellation for active tasks and cancels queued futures.
         * @note A call from a worker only signals shutdown. An external owner
         * performs the final joins through shutdown() or destruction.
         */
        void shutdown(ShutdownMode mode = ShutdownMode::CANCEL_PENDING) noexcept
        {
            // A worker may request shutdown, but it must never destroy or
            // join its own jthread. The owning thread performs the final join
            // when the pool is destroyed or shutdown is called externally.
            if (is_worker_thread())
            {
                signal_shutdown(mode);
                return;
            }

            const std::lock_guard shutdown_lock{shutdown_mutex_};
            signal_shutdown(mode);

            std::vector<std::jthread> workers_to_join;
            {
                const std::lock_guard lock{workers_mutex_};
                workers_to_join.swap(workers_);
            }
            for (auto& worker : workers_to_join)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
        }

        ~IndustrialWorkerPool() noexcept { shutdown(); }

    private:
        [[nodiscard]] bool is_worker_thread() const noexcept
        {
            return current_worker_pool_ == this;
        }

        void signal_shutdown(ShutdownMode mode) noexcept
        {
            {
                const std::lock_guard producer_lock{producer_mutex_};
                queue_state_.fetch_or(stopping_mask_, std::memory_order_release);
            }

            if (mode == ShutdownMode::CANCEL_PENDING)
            {
                static_cast<void>(cancel_pending());
                const std::lock_guard workers_lock{workers_mutex_};
                for (auto& worker : workers_) worker.request_stop();
            }

            queue_state_.notify_all();
        }

        void run(std::stop_token stop_token)
        {
            current_worker_pool_ = this;
            while (true)
            {
                auto state = queue_state_.load(std::memory_order_acquire);
                while (pending_from_state(state) == 0 && !is_stopping_state(state))
                {
                    waiting_workers_.fetch_add(1, std::memory_order_relaxed);
                    const auto observed = queue_state_.load(std::memory_order_acquire);
                    if (pending_from_state(observed) == 0 &&
                        !is_stopping_state(observed))
                        queue_state_.wait(observed, std::memory_order_acquire);
                    waiting_workers_.fetch_sub(1, std::memory_order_relaxed);
                    state = queue_state_.load(std::memory_order_acquire);
                }

                if (pending_from_state(state) == 0 && is_stopping_state(state))
                {
                    current_worker_pool_ = nullptr;
                    return;
                }

                std::optional<TaskItem> item;
                std::unique_lock consumer_lock{consumer_mutex_};
                state = queue_state_.load(std::memory_order_acquire);
                if (pending_from_state(state) == 0)
                    continue;
                detail::helgrind_happens_after(&queue_state_);
                if (tasks_[queue_head_]->bulk)
                {
                    process_bulk(stop_token, std::move(consumer_lock), state);
                    continue;
                }
                item.emplace(std::move(*tasks_[queue_head_]));
                tasks_[queue_head_].reset();
                queue_head_ = next_queue_index(queue_head_);
                detail::helgrind_happens_before(&queue_state_);
                queue_state_.fetch_add(active_unit_ - 1, std::memory_order_acq_rel);
                consumer_lock.unlock();

                if (waiting_producers_.load(std::memory_order_relaxed) != 0)
                    queue_state_.notify_one();
                execute(*item, stop_token, false);
                finish_task();
            }
        }

        void process_bulk(
            std::stop_token stop_token,
            std::unique_lock<std::mutex> consumer_lock,
            std::size_t state)
        {
            std::array<std::optional<TaskItem>, bulk_claim_size_> batch;
            std::size_t claimed = 0;
            const auto limit = std::min(
                bulk_claim_size_, pending_from_state(state));
            while (claimed < limit && tasks_[queue_head_]->bulk)
            {
                batch[claimed].emplace(std::move(*tasks_[queue_head_]));
                tasks_[queue_head_].reset();
                queue_head_ = next_queue_index(queue_head_);
                ++claimed;
            }
            detail::helgrind_happens_before(&queue_state_);
            queue_state_.fetch_add(
                claimed * (active_unit_ - 1), std::memory_order_acq_rel);
            consumer_lock.unlock();

            if (waiting_producers_.load(std::memory_order_relaxed) != 0)
                queue_state_.notify_one();
            for (std::size_t index = 0; index < claimed; ++index)
                execute(*batch[index], stop_token, false);
            finish_tasks(claimed);
        }

        void execute(
            TaskItem& item, std::stop_token stop_token, bool finish = true) noexcept
        {
            std::optional<OperationResult> task_result;
            std::exception_ptr task_exception;
            try
            {
                if (std::holds_alternative<Task>(item.callback))
                {
                    task_result.emplace(std::get<Task>(item.callback)());
                }
                else
                {
                    task_result.emplace(
                        std::get<CancellableTask>(item.callback)(stop_token));
                }
            }
            catch (...)
            {
                task_exception = std::current_exception();
            }
            if (!item.completion)
            {
                if (task_exception || !*task_result)
                {
                    failed_dispatches_.fetch_add(1, std::memory_order_relaxed);
                }
                if (finish) finish_task();
                return;
            }

            try
            {
                if (task_exception)
                {
                    item.completion->set_exception(task_exception);
                }
                else
                {
                    item.completion->set_value(std::move(*task_result));
                }
            }
            catch (...)
            {
                // Each tracked TaskItem owns one promise whose future is
                // retrieved exactly once before enqueueing.
                std::terminate();
            }
            if (finish) finish_task();
        }

        void finish_task() noexcept
        {
            finish_tasks(1);
        }

        void finish_tasks(std::size_t count) noexcept
        {
            detail::helgrind_happens_before(&queue_state_);
            const auto previous_state =
                queue_state_.fetch_sub(count * active_unit_, std::memory_order_release);
            if (active_from_state(previous_state) == count &&
                pending_from_state(previous_state) == 0)
                notify_idle_waiters();
        }

        [[nodiscard]] bool is_idle() const noexcept
        {
            const auto state = queue_state_.load(std::memory_order_acquire);
            return pending_from_state(state) == 0 &&
                   active_from_state(state) == 0;
        }

        void notify_idle_waiters() noexcept
        {
            if (waiting_for_idle_.load(std::memory_order_relaxed) == 0) return;
            completion_epoch_.fetch_add(1, std::memory_order_release);
            completion_epoch_.notify_all();
        }

        [[nodiscard]] static std::size_t validate_queue_capacity(std::size_t capacity)
        {
            if (capacity == 0 || capacity > max_queue_capacity)
            {
                throw std::invalid_argument("Worker pool queue capacity must be in [1, 1024]");
            }

            return capacity;
        }

        [[nodiscard]] std::size_t next_queue_index(std::size_t index) const noexcept
        {
            return index + 1 == queue_capacity_ ? 0 : index + 1;
        }

        [[nodiscard]] static std::size_t pending_from_state(std::size_t state) noexcept
        {
            return state & pending_mask_;
        }

        [[nodiscard]] static bool is_stopping_state(std::size_t state) noexcept
        {
            return (state & stopping_mask_) != 0;
        }

        [[nodiscard]] static std::size_t active_from_state(std::size_t state) noexcept
        {
            return (state & active_mask_) >> active_shift_;
        }

        [[nodiscard]] std::size_t cancel_pending() noexcept
        {
            const std::scoped_lock queue_locks{producer_mutex_, consumer_mutex_};
            const auto cancelled = pending_from_state(
                queue_state_.load(std::memory_order_acquire));
            detail::helgrind_happens_after(&queue_state_);
            auto remaining = cancelled;
            while (remaining != 0)
            {
                auto& item = tasks_[queue_head_];
                if (item->completion)
                {
                    try
                    {
                        item->completion->set_value(
                            std::unexpected(task_cancelled_error()));
                    }
                    catch (...)
                    {
                        // Every tracked queue slot owns a valid promise with one future.
                        std::terminate();
                    }
                }
                else
                {
                    cancelled_dispatches_.fetch_add(1, std::memory_order_relaxed);
                }

                item.reset();
                queue_head_ = next_queue_index(queue_head_);
                --remaining;
            }
            queue_tail_ = queue_head_;

            if (cancelled != 0)
            {
                detail::helgrind_happens_before(&queue_state_);
                queue_state_.fetch_sub(cancelled, std::memory_order_release);
                if (active_from_state(queue_state_.load(std::memory_order_acquire)) == 0)
                    notify_idle_waiters();
                queue_state_.notify_all();
            }
            return cancelled;
        }

        static constexpr std::size_t count_bits_ = 11;
        static constexpr std::size_t bulk_claim_size_ = 16;
        static constexpr std::size_t active_shift_ = count_bits_;
        static constexpr auto stopping_mask_ =
            std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
        static constexpr auto pending_mask_ = (std::size_t{1} << count_bits_) - 1;
        static constexpr auto active_unit_ = std::size_t{1} << active_shift_;
        static constexpr auto active_mask_ = pending_mask_ << active_shift_;
        static_assert(max_queue_capacity <= pending_mask_);
        static_assert(max_worker_count <= pending_mask_);
        static_assert((active_mask_ & stopping_mask_) == 0);

        mutable std::mutex producer_mutex_;
        mutable std::mutex consumer_mutex_;
        std::vector<std::jthread> workers_;
        std::mutex workers_mutex_;
        std::mutex shutdown_mutex_;
        std::size_t worker_count_ = 0;
        std::size_t queue_capacity_ = max_queue_capacity;
        std::vector<std::optional<TaskItem>> tasks_;
        std::size_t queue_head_ = 0;
        std::size_t queue_tail_ = 0;
        std::atomic<std::size_t> queue_state_ = 0;
        std::atomic<std::size_t> completion_epoch_ = 0;
        std::atomic<std::size_t> waiting_for_idle_ = 0;
        std::atomic<std::size_t> waiting_producers_ = 0;
        std::atomic<std::size_t> waiting_workers_ = 0;
        std::atomic<std::size_t> failed_dispatches_ = 0;
        std::atomic<std::size_t> cancelled_dispatches_ = 0;
        inline static thread_local IndustrialWorkerPool* current_worker_pool_ = nullptr;
    };

    /** @brief Unified public name for the bounded owning worker pool. */
    using WorkerPool = IndustrialWorkerPool;
}
