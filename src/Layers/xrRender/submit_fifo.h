// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// One thread that feeds a GPU queue in the order the jobs were pushed, shared by the DX12 and Vulkan layers.
// Queue submissions and presents are driver calls of hundreds of microseconds whose result nobody needs on the
// spot: the game thread reserves the fence value, pushes the job and goes on with the next frame.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace xrsubmit
{
template <class Job>
class Fifo
{
public:
    ~Fifo() { Stop(); }

    template <class Run>
    void Start(Run run, const wchar_t* name)
    {
        if (m_thread.joinable())
            return;
        m_stop = false;
        m_thread = std::thread([this, run, name]() mutable {
            SetThreadDescription(GetCurrentThread(), name);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            Loop(run);
        });
        m_running.store(true, std::memory_order_release);
    }

    // Everything pushed so far runs before the thread ends.
    void Stop()
    {
        if (!m_thread.joinable())
            return;
        {
            std::lock_guard lock(m_lock);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
        m_running.store(false, std::memory_order_release);
    }

    bool Running() const { return m_running.load(std::memory_order_acquire); }

    // Swaps `job` into the queue: the caller gets back an old job whose containers keep their capacity.
    void Push(Job& job)
    {
        {
            std::unique_lock lock(m_lock);
            m_idle.wait(lock, [this] { return m_pushed - m_done < kCapacity; });
            std::swap(m_ring[m_pushed % kCapacity], job);
            ++m_pushed;
        }
        m_wake.notify_one();
    }

    // Blocks until every job pushed before the call has finished.
    void Drain()
    {
        if (!Running() || std::this_thread::get_id() == m_thread.get_id())
            return;
        std::unique_lock lock(m_lock);
        const uint64_t target = m_pushed;
        m_idle.wait(lock, [&] { return m_done >= target; });
    }

private:
    static constexpr uint64_t kCapacity = 64;

    template <class Run>
    void Loop(Run& run)
    {
        for (;;)
        {
            Job* job = nullptr;
            {
                std::unique_lock lock(m_lock);
                m_wake.wait(lock, [this] { return m_stop || m_done != m_pushed; });
                if (m_done == m_pushed)
                    return;
                job = &m_ring[m_done % kCapacity];
            }
            run(*job);
            {
                std::lock_guard lock(m_lock);
                ++m_done;
            }
            m_idle.notify_all();
        }
    }

    Job m_ring[kCapacity];
    uint64_t m_pushed = 0, m_done = 0;
    bool m_stop = false;
    std::atomic<bool> m_running{false};
    std::mutex m_lock;
    std::condition_variable m_wake, m_idle;
    std::thread m_thread;
};
} // namespace xrsubmit
