#ifndef RIPPLE_BASICS_THREADLOCALQUEUE_H_INCLUDED
#define RIPPLE_BASICS_THREADLOCALQUEUE_H_INCLUDED

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <set>

namespace ripple {
template <typename T>
class ThreadLocalQueue
{
private:
    static inline thread_local std::vector<T> local_queue;
    static inline thread_local size_t local_count{0};

    mutable std::recursive_mutex mutex;
    std::set<std::vector<T>*> thread_queues;
    std::atomic<size_t> total_count{0};

public:
    void
    register_thread() noexcept
    {
        std::lock_guard lock(mutex);
        thread_queues.emplace(&local_queue);
        std::cout << "Thread registered: " << reinterpret_cast<uint64_t>(&local_queue) << "\n";
        local_count = 0;
    }

    void
    unregister_thread() noexcept
    {
        std::lock_guard lock(mutex);
        auto it =
            std::find(thread_queues.begin(), thread_queues.end(), &local_queue);
        if (it != thread_queues.end())
        {
            thread_queues.erase(it);
        }
        total_count.fetch_sub(local_count, std::memory_order_relaxed);
        local_queue.clear();
        local_count = 0;
    }

    void
    push_back(T item) noexcept
    {
        static thread_local bool registered = false;
        if (!registered)
        {
            register_thread();
            registered = true;
        }
        std::lock_guard lock(mutex);
        local_queue.push_back(std::move(item));
        local_count++;
        total_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<T>
    merge_and_clear() noexcept
    {
        std::lock_guard lock(mutex);

        size_t expected_size = total_count.load(std::memory_order_relaxed);
        std::vector<T> merged;
        merged.reserve(expected_size);

        for (auto* queue : thread_queues)
        {
            merged.insert(
                merged.end(),
                std::make_move_iterator(queue->begin()),
                std::make_move_iterator(queue->end()));
            queue->clear();
            queue->shrink_to_fit();
        }

        total_count.store(0, std::memory_order_relaxed);
        for (auto* queue : thread_queues)
        {
            *const_cast<size_t*>(&local_count) = 0;
        }

        return merged;
    }

    size_t
    local_size() const noexcept
    {
        std::lock_guard lock(mutex);
        return local_count;
    }

    size_t
    size() const noexcept
    {
        std::lock_guard lock(mutex);
        return total_count.load(std::memory_order_relaxed);
    }

    bool
    empty() const noexcept
    {
        std::lock_guard lock(mutex);
        return total_count.load(std::memory_order_relaxed) == 0;
    }

    void
    swap(ThreadLocalQueue& other) noexcept
    {
        if (this == &other)
            return;

        // With recursive mutex, we can simply lock both mutexes sequentially
        {
            std::scoped_lock(mutex, other.mutex);

            // Swap the thread queues
            thread_queues.swap(other.thread_queues);

            // Swap the total counts using atomic exchange
            size_t this_count = total_count.load(std::memory_order_relaxed);
            size_t other_count = other.total_count.load(std::memory_order_relaxed);
            total_count.store(other_count, std::memory_order_relaxed);
            other.total_count.store(this_count, std::memory_order_relaxed);
        }
    }

    friend void
    swap(ThreadLocalQueue& lhs, ThreadLocalQueue& rhs) noexcept
    {
        lhs.swap(rhs);
    }

    void
    swap(std::vector<T>& vec) noexcept
    {
        std::lock_guard lock(mutex);

        std::vector<T> temp;
        temp.swap(vec);

        // Move all our queue contents into vec
        size_t total_size = total_count.load(std::memory_order_relaxed);

        vec.reserve(total_size);

        // For each queue
        int i = 0;
        for (auto* queue : thread_queues)
        {
            // Move elements one at a time to handle non-copyable types
            while (!queue->empty())
            {
                vec.push_back(std::move(queue->back()));
                queue->pop_back();
            }
            queue->shrink_to_fit();
        }

        // Move temp's contents to the first available queue
        if (!thread_queues.empty())
        {
            auto* first_queue = *thread_queues.begin();
            // Move elements one at a time from temp to queue
            while (!temp.empty())
            {
                first_queue->push_back(std::move(temp.back()));
                temp.pop_back();
            }
            local_count = first_queue->size();
            total_count.store(local_count, std::memory_order_relaxed);
        }
        else
        {
            total_count.store(0, std::memory_order_relaxed);
        }
    }

    friend void
    swap(ThreadLocalQueue& tlq, std::vector<T>& vec) noexcept
    {
        tlq.swap(vec);
    }

    friend void
    swap(std::vector<T>& vec, ThreadLocalQueue& tlq) noexcept
    {
        tlq.swap(vec);
    }
};

}  // namespace ripple
#endif  // THREAD_LOCAL_QUEUE_HPP
