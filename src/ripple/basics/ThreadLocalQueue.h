#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define DEBUG_TLQ 0

namespace detail {

enum class Operation { PUSH_BACK, MERGE_AND_CLEAR, SIZE, REGISTER, UNREGISTER };

// Thread-specific data including mutex and vector
template <typename T>
struct ThreadData
{
    std::mutex mutex;
    std::vector<T> queue;
};

// Structure to hold per-instance data
template <typename T>
struct InstanceData
{
    std::mutex mutex;
    std::atomic<size_t> total_size{0};
    std::unordered_map<std::thread::id, std::unique_ptr<ThreadData<T>>>
        thread_queues;
};

// Global map of instance IDs to their data
template <typename T>
struct GlobalInstances
{
    std::mutex mutex;
    std::unordered_map<uintptr_t, std::shared_ptr<InstanceData<T>>> instances;

    static GlobalInstances&
    instance()
    {
        static GlobalInstances inst;
        return inst;
    }
};

// Helper to get or create instance data
template <typename T>
std::shared_ptr<InstanceData<T>>
get_instance_data(uintptr_t instance_id)
{
    auto& global = GlobalInstances<T>::instance();
    std::lock_guard<std::mutex> lock(global.mutex);
    auto it = global.instances.find(instance_id);
    if (it == global.instances.end())
    {
        auto data = std::make_shared<InstanceData<T>>();
        global.instances[instance_id] = data;
        return data;
    }
    return it->second;
}

// Generic dispatch function that handles all operations
template <typename T>
std::pair<std::vector<T>, size_t>
dispatch(uintptr_t instance_id, Operation op, T* item = nullptr)
{
    auto instance_data = get_instance_data<T>(instance_id);
    auto thread_id = std::this_thread::get_id();

    switch (op)
    {
        case Operation::MERGE_AND_CLEAR: {
            auto current_size =
                instance_data->total_size.load(std::memory_order_acquire);
            if (DEBUG_TLQ)
                std::cout << "start merge, counter=" << current_size << "\n";

            if (current_size == 0)
            {
                return {std::vector<T>(), 0};
            }

            std::vector<T> result;
            size_t total_size = 0;

            // Lock the entire instance data to prevent concurrent modifications
            std::lock_guard<std::mutex> instance_lock(instance_data->mutex);

            // First calculate total size
            for (const auto& queue_entry : instance_data->thread_queues)
            {
                auto queue_size = queue_entry.second->queue.size();
                total_size += queue_size;
                if (DEBUG_TLQ)
                    std::cout << "queue size for thread " << queue_entry.first
                              << ": " << queue_size << "\n";
            }

            if (DEBUG_TLQ)
                std::cout << "merge total size: " << total_size << "\n";
            result.reserve(total_size);

            // Move all elements from all thread queues
            for (auto& queue_entry : instance_data->thread_queues)
            {
                auto& queue = queue_entry.second->queue;
                std::lock_guard<std::mutex> queue_lock(
                    queue_entry.second->mutex);

                // For non-movable types, we need to copy elements individually
                for (auto& item : queue)
                {
                    result.push_back(
                        item);  // Will use copy constructor instead of move
                }
                if (DEBUG_TLQ)
                    std::cout << "copied queue of size: " << queue.size()
                              << " for thread " << queue_entry.first << "\n";
                queue.clear();
            }

            instance_data->total_size.store(0, std::memory_order_release);
            if (DEBUG_TLQ)
                std::cout << "merge size: " << result.size() << ", counter: "
                          << instance_data->total_size.load(
                                 std::memory_order_acquire)
                          << "\n";
            return {std::move(result), 0};
        }

        case Operation::REGISTER: {
            std::lock_guard<std::mutex> lock(instance_data->mutex);
            if (instance_data->thread_queues.find(thread_id) ==
                instance_data->thread_queues.end())
            {
                instance_data->thread_queues[thread_id] =
                    std::make_unique<ThreadData<T>>();
            }
            return {std::vector<T>(), 0};
        }

        case Operation::PUSH_BACK: {
            // Ensure thread is registered
            {
                std::lock_guard<std::mutex> lock(instance_data->mutex);
                if (instance_data->thread_queues.find(thread_id) ==
                    instance_data->thread_queues.end())
                {
                    instance_data->thread_queues[thread_id] =
                        std::make_unique<ThreadData<T>>();
                }
            }

            auto& thread_data = instance_data->thread_queues[thread_id];
            std::lock_guard<std::mutex> lock(thread_data->mutex);

            if (item)
            {
                thread_data->queue.push_back(std::move(*item));
                instance_data->total_size.fetch_add(
                    1, std::memory_order_release);
            }

            if (DEBUG_TLQ)
                std::cout << "pushed, counter="
                          << instance_data->total_size.load(
                                 std::memory_order_acquire)
                          << "\n";
            return {std::vector<T>(), 0};
        }

        case Operation::SIZE: {
            return {
                std::vector<T>(),
                instance_data->total_size.load(std::memory_order_acquire)};
        }

        case Operation::UNREGISTER: {
            std::lock_guard<std::mutex> lock(instance_data->mutex);
            if (auto it = instance_data->thread_queues.find(thread_id);
                it != instance_data->thread_queues.end())
            {
                size_t queue_size = it->second->queue.size();
                instance_data->total_size.fetch_sub(
                    queue_size, std::memory_order_release);
                instance_data->thread_queues.erase(it);
            }
            return {std::vector<T>(), 0};
        }

        default:
            return {std::vector<T>(), 0};
    }
}

}  // namespace detail

template <typename T>
class ThreadLocalQueue
{
public:
    ThreadLocalQueue() : instance_id(reinterpret_cast<uintptr_t>(this))
    {
        // Initial registration happens in the constructor thread
        detail::dispatch<T>(instance_id, detail::Operation::REGISTER);
    }

    ~ThreadLocalQueue()
    {
        // Clean up instance data when the queue is destroyed
        auto& global = detail::GlobalInstances<T>::instance();
        std::lock_guard<std::mutex> lock(global.mutex);
        global.instances.erase(instance_id);
    }

    // Prevent copying to avoid counter complications
    ThreadLocalQueue(const ThreadLocalQueue&) = delete;
    ThreadLocalQueue&
    operator=(const ThreadLocalQueue&) = delete;

    // Move operations
    ThreadLocalQueue(ThreadLocalQueue&& other) noexcept
        : instance_id(other.instance_id)
    {
        other.instance_id = 0;
    }

    ThreadLocalQueue&
    operator=(ThreadLocalQueue&& other) noexcept
    {
        if (this != &other)
        {
            auto& global = detail::GlobalInstances<T>::instance();
            std::lock_guard<std::mutex> lock(global.mutex);
            global.instances.erase(instance_id);
            instance_id = other.instance_id;
            other.instance_id = 0;
        }
        return *this;
    }

    // Explicitly register current thread
    void
    register_thread() noexcept
    {
        detail::dispatch<T>(instance_id, detail::Operation::REGISTER);
    }

    void
    unregister_thread() noexcept
    {
        detail::dispatch<T>(instance_id, detail::Operation::UNREGISTER);
    }

    void
    push_back(T item) noexcept
    {
        detail::dispatch<T>(instance_id, detail::Operation::PUSH_BACK, &item);
    }

    std::vector<T>
    merge_and_clear() noexcept
    {
        return detail::dispatch<T>(
                   instance_id, detail::Operation::MERGE_AND_CLEAR)
            .first;
    }

    size_t
    size() const noexcept
    {
        return detail::dispatch<T>(instance_id, detail::Operation::SIZE).second;
    }

    bool
    empty() const noexcept
    {
        return size() == 0;
    }

    void
    swap(std::vector<T>& other) noexcept
    {
        auto merged = merge_and_clear();
        merged.swap(other);
    }

    void
    swap(ThreadLocalQueue& other) noexcept
    {
        if (this == &other)
        {
            return;
        }

        auto& global = detail::GlobalInstances<T>::instance();

        // Get the global lock first
        std::lock_guard<std::mutex> global_lock(global.mutex);

        // Then get individual instance locks in a consistent order
        auto my_instance = global.instances[instance_id];
        auto other_instance = global.instances[other.instance_id];

        std::lock_guard<std::mutex> lock1(my_instance->mutex);
        std::lock_guard<std::mutex> lock2(other_instance->mutex);

        // Now safe to swap instance IDs and data
        std::swap(instance_id, other.instance_id);
        std::swap(
            global.instances[instance_id], global.instances[other.instance_id]);
    }

    friend void
    swap(ThreadLocalQueue& lhs, ThreadLocalQueue& rhs) noexcept
    {
        lhs.swap(rhs);
    }

private:
    uintptr_t instance_id;
};
