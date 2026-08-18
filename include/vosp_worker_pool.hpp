#pragma once

#include "vosp_error.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vosp::async
{
    using vosp::error::Category;
    using vosp::error::Error;
    using vosp::error::OperationResult;

    inline constexpr std::size_t max_worker_count = 1024;
    inline constexpr std::size_t max_queue_capacity = 1024;
    inline constexpr std::uint32_t task_cancelled_code = 0xE004;

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
                {
                    const std::lock_guard lock{mutex_};
                    stopping_ = true;
                }
                task_available_.notify_all();
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
         * @note Uses grouped queue refills to reduce producer-side lock contention.
         */
        [[nodiscard]] std::size_t dispatch_bulk(std::span<Task> tasks)
        {
            std::size_t accepted = 0;
            while (accepted < tasks.size())
            {
                std::size_t enqueued = 0;
                {
                    std::unique_lock lock{mutex_};
                    const auto remaining = tasks.size() - accepted;
                    const auto refill_size = std::min(
                        remaining,
                        std::max<std::size_t>(1, std::min<std::size_t>(32, queue_capacity_ / 4)));
                    space_available_.wait(lock, [this, refill_size]
                    {
                        return stopping_ ||
                               queue_capacity_ - pending_tasks_ >= refill_size;
                    });

                    if (stopping_)
                    {
                        return accepted;
                    }

                    enqueued = std::min(remaining, queue_capacity_ - pending_tasks_);
                    for (std::size_t index = 0; index < enqueued; ++index)
                    {
                        tasks_[queue_tail_].emplace(
                            TaskItem{std::move(tasks[accepted + index]), std::nullopt});
                        queue_tail_ = next_queue_index(queue_tail_);
                        ++pending_tasks_;
                    }
                    accepted += enqueued;
                }

                if (enqueued == 1)
                {
                    task_available_.notify_one();
                }
                else
                {
                    task_available_.notify_all();
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
            std::unique_lock lock{mutex_};
            idle_.wait(lock, [this]
            {
                return pending_tasks_ == 0 &&
                       active_tasks_.load(std::memory_order_relaxed) == 0;
            });
        }

    private:
        struct TaskItem
        {
            template<typename Callback, typename CompletionTag>
                requires std::same_as<std::remove_cvref_t<Callback>, Task> ||
                         std::same_as<std::remove_cvref_t<Callback>, CancellableTask>
            TaskItem(Callback&& value, CompletionTag completion_tag)
                : callback{std::in_place_type<std::remove_cvref_t<Callback>>,
                           std::forward<Callback>(value)},
                  completion{completion_tag}
            {
            }

            std::variant<Task, CancellableTask> callback;
            std::optional<std::promise<OperationResult>> completion;
        };

        void enqueue(TaskItem item)
        {
            {
                std::unique_lock lock{mutex_};
                space_available_.wait(lock, [this]
                {
                    return stopping_ || pending_tasks_ < queue_capacity_;
                });

                if (stopping_)
                {
                    throw std::runtime_error("Cannot submit task after worker pool shutdown");
                }

                tasks_[queue_tail_].emplace(std::move(item));
                queue_tail_ = next_queue_index(queue_tail_);
                ++pending_tasks_;
            }

            task_available_.notify_one();
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
            const std::lock_guard lock{mutex_};
            return pending_tasks_;
        }

        /** @brief Returns whether the pool rejects new submissions. */
        [[nodiscard]] bool is_stopping() const noexcept
        {
            const std::lock_guard lock{mutex_};
            return stopping_;
        }

        /** @brief Returns the number of tasks currently executing. */
        [[nodiscard]] std::size_t active_tasks() const noexcept
        {
            return active_tasks_.load(std::memory_order_relaxed);
        }

        /**
         * @brief Cancels all tasks that have not started.
         * @return Number of cancelled queued tasks.
         */
        [[nodiscard]] std::size_t clear_queue() noexcept
        {
            std::size_t cancelled = 0;
            {
                const std::lock_guard lock{mutex_};
                cancelled = cancel_pending_locked();
            }

            space_available_.notify_all();
            idle_.notify_all();
            return cancelled;
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
                const std::lock_guard lock{mutex_};
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
                const std::lock_guard lock{mutex_};
                stopping_ = true;
                if (mode == ShutdownMode::CANCEL_PENDING)
                {
                    static_cast<void>(cancel_pending_locked());
                    for (auto& worker : workers_)
                    {
                        worker.request_stop();
                    }
                }
            }

            task_available_.notify_all();
            space_available_.notify_all();
            idle_.notify_all();
        }

        void run(std::stop_token stop_token)
        {
            current_worker_pool_ = this;
            while (true)
            {
                std::optional<TaskItem> item;
                {
                    std::unique_lock lock{mutex_};
                    task_available_.wait(lock, [this]
                    {
                        return stopping_ || pending_tasks_ != 0;
                    });

                    if (pending_tasks_ == 0)
                    {
                        if (stopping_)
                        {
                            current_worker_pool_ = nullptr;
                            return;
                        }

                        continue;
                    }

                    item.emplace(std::move(*tasks_[queue_head_]));
                    tasks_[queue_head_].reset();
                    queue_head_ = next_queue_index(queue_head_);
                    --pending_tasks_;
                    active_tasks_.fetch_add(1, std::memory_order_relaxed);
                }

                space_available_.notify_one();
                execute(*item, stop_token);
            }
        }

        void execute(TaskItem& item, std::stop_token stop_token) noexcept
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
                finish_task();
                return;
            }

            finish_task();

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
        }

        void finish_task() noexcept
        {
            if (active_tasks_.fetch_sub(1, std::memory_order_relaxed) == 1)
            {
                const std::lock_guard lock{mutex_};
                if (pending_tasks_ == 0)
                {
                    idle_.notify_all();
                }
            }
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

        [[nodiscard]] std::size_t cancel_pending_locked() noexcept
        {
            const auto cancelled = pending_tasks_;
            while (pending_tasks_ != 0)
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
                --pending_tasks_;
            }
            queue_tail_ = queue_head_;
            return cancelled;
        }

        mutable std::mutex mutex_;
        std::condition_variable task_available_;
        std::condition_variable space_available_;
        std::condition_variable idle_;
        std::vector<std::jthread> workers_;
        std::mutex shutdown_mutex_;
        std::size_t worker_count_ = 0;
        std::size_t queue_capacity_ = max_queue_capacity;
        std::vector<std::optional<TaskItem>> tasks_;
        std::size_t queue_head_ = 0;
        std::size_t queue_tail_ = 0;
        std::size_t pending_tasks_ = 0;
        std::atomic<std::size_t> active_tasks_ = 0;
        std::atomic<std::size_t> failed_dispatches_ = 0;
        std::atomic<std::size_t> cancelled_dispatches_ = 0;
        bool stopping_ = false;
        inline static thread_local IndustrialWorkerPool* current_worker_pool_ = nullptr;
    };
}
