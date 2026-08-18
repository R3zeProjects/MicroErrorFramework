#pragma once

#include "vosp_error.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
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
     * The pool has a hard limit of 1024 workers and 1024 queued tasks.
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
            : queue_capacity_{queue_capacity}
        {
            if (worker_count == 0)
            {
                worker_count = std::thread::hardware_concurrency();
            }

            if (worker_count == 0 || worker_count > max_worker_count)
            {
                throw std::invalid_argument("Worker pool worker count must be in [1, 1024]");
            }

            if (queue_capacity == 0 || queue_capacity > max_queue_capacity)
            {
                throw std::invalid_argument("Worker pool queue capacity must be in [1, 1024]");
            }

            worker_count_ = worker_count;
            workers_.reserve(worker_count);
            for (std::size_t index = 0; index < worker_count; ++index)
            {
                workers_.emplace_back([this](std::stop_token stop_token)
                {
                    run(stop_token);
                });
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
            return submit_cancellable(
                [task = std::move(task)](std::stop_token) mutable
                {
                    return task();
                });
        }

        /**
         * @brief Submits a task that can cooperatively observe cancellation.
         * @param task Callback that must check stop_token and return promptly.
         * @return Future containing the task result.
         */
        [[nodiscard]] std::future<OperationResult> submit_cancellable(CancellableTask task)
        {
            TaskItem item{std::move(task)};
            std::future<OperationResult> result = item.result.get_future();

            {
                std::unique_lock lock{mutex_};
                space_available_.wait(lock, [this]
                {
                    return stopping_ || tasks_.size() < queue_capacity_;
                });

                if (stopping_)
                {
                    throw std::runtime_error("Cannot submit task after worker pool shutdown");
                }

                tasks_.push_back(std::move(item));
            }

            task_available_.notify_one();
            return result;
        }

        [[nodiscard]] std::size_t worker_count() const noexcept { return worker_count_; }
        [[nodiscard]] std::size_t queue_capacity() const noexcept { return queue_capacity_; }

        /** @brief Returns the number of tasks waiting to start. */
        [[nodiscard]] std::size_t pending_tasks() const noexcept
        {
            const std::lock_guard lock{mutex_};
            return tasks_.size();
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
            const std::lock_guard lock{mutex_};
            return active_tasks_;
        }

        /**
         * @brief Cancels all tasks that have not started.
         * @return Number of cancelled queued tasks.
         */
        [[nodiscard]] std::size_t clear_queue() noexcept
        {
            std::deque<TaskItem> cancelled;
            {
                const std::lock_guard lock{mutex_};
                cancelled.swap(tasks_);
            }

            space_available_.notify_all();
            for (auto& item : cancelled)
            {
                try
                {
                    item.result.set_value(std::unexpected(task_cancelled_error()));
                }
                catch (...)
                {
                    // A queued item always owns a valid promise with a
                    // retrieved future. Reaching this branch means that
                    // invariant was violated; continuing would silently
                    // leave the caller with a broken future.
                    std::terminate();
                }
            }

            return cancelled.size();
        }

        /**
         * @brief Stops the pool and joins all workers.
         * @param mode Drain queued tasks or cancel them before joining.
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
            workers_to_join.clear();
        }

        ~IndustrialWorkerPool() noexcept { shutdown(); }

    private:
        struct TaskItem
        {
            explicit TaskItem(CancellableTask value) : task{std::move(value)} {}

            CancellableTask task;
            std::promise<OperationResult> result;
        };

        [[nodiscard]] bool is_worker_thread() const noexcept
        {
            return current_worker_pool_ == this;
        }

        void signal_shutdown(ShutdownMode mode) noexcept
        {
            {
                const std::lock_guard lock{mutex_};
                stopping_ = true;
            }

            if (mode == ShutdownMode::CANCEL_PENDING)
            {
                static_cast<void>(clear_queue());
            }

            {
                const std::lock_guard lock{mutex_};
                for (auto& worker : workers_)
                {
                    worker.request_stop();
                }
            }

            task_available_.notify_all();
            space_available_.notify_all();
        }

        void run(std::stop_token stop_token)
        {
            current_worker_pool_ = this;
            while (true)
            {
                TaskItem item{CancellableTask{}};
                {
                    std::unique_lock lock{mutex_};
                    task_available_.wait(lock, stop_token, [this]
                    {
                        return stopping_ || !tasks_.empty();
                    });

                    if (tasks_.empty())
                    {
                        if (stopping_)
                        {
                            current_worker_pool_ = nullptr;
                            return;
                        }

                        continue;
                    }

                    item = std::move(tasks_.front());
                    tasks_.pop_front();
                    ++active_tasks_;
                }

                space_available_.notify_one();
                try
                {
                    item.result.set_value(item.task(stop_token));
                }
                catch (...)
                {
                    item.result.set_exception(std::current_exception());
                }

                {
                    const std::lock_guard lock{mutex_};
                    --active_tasks_;
                }
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable_any task_available_;
        std::condition_variable_any space_available_;
        std::deque<TaskItem> tasks_;
        std::vector<std::jthread> workers_;
        std::mutex shutdown_mutex_;
        std::size_t worker_count_ = 0;
        std::size_t queue_capacity_ = max_queue_capacity;
        std::size_t active_tasks_ = 0;
        bool stopping_ = false;
        inline static thread_local IndustrialWorkerPool* current_worker_pool_ = nullptr;
    };
}
