#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <future>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <memory>

// Add above ThreadPool class
enum class TaskPriority { High = 0, Normal = 1, Low = 2, Count = 3 };

// L: Work-stealing thread pool.
// Each worker owns a local deque. 'enqueue' round-robins tasks across workers.
// Idle workers steal from the back of a neighbour's deque, which keeps
// recently-enqueued (hot) tasks in the owner thread while spreading older
// (cold) work to stealers — reducing single-queue lock contention under burst load.
class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads)
		: stop_(false), nextQueue_(0), numWorkers_(numThreads), taskCount_(0)
	{
		queues_.resize(numThreads);
		for (size_t i = 0; i < numThreads; ++i)
			queues_[i] = std::make_unique<LocalQueue>();
		for (size_t i = 0; i < numThreads; ++i)
			workers_.emplace_back([this, i]
								  { workerLoop(i); });
	}

	~ThreadPool()
	{
		{
			std::lock_guard<std::mutex> lk(sleepMux_);
			stop_.store(true);
		}
		cv_.notify_all();
		for (auto &w : workers_)
			w.join();
	}

	template <class F>
	auto enqueue(TaskPriority priority, F &&f) -> std::future<void>
	{
		auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
		std::future<void> res = task->get_future();
		// Distribute round-robin across worker queues
		size_t qi = nextQueue_.fetch_add(1, std::memory_order_relaxed) % numWorkers_;
		{
			std::lock_guard<std::mutex> lk(queues_[qi]->mx);
			queues_[qi]->dq[static_cast<int>(priority)].emplace_front([task]
										  { (*task)(); }); // LIFO push
		}
		taskCount_.fetch_add(1, std::memory_order_release);
		cv_.notify_one();
		return res;
	}

	// Overload for backward compatibility (defaults to Normal)
	template <class F>
	auto enqueue(F &&f) -> std::future<void>
	{
		return enqueue(TaskPriority::Normal, std::forward<F>(f));
	}

private:
	struct LocalQueue
	{
		std::deque<std::function<void()>> dq[static_cast<int>(TaskPriority::Count)];
		std::mutex mx;
	};

	// Try to obtain a task: own queue front (LIFO — cache-warm), then steal from
	// another worker's back (FIFO — oldest items first, minimises latency).
	std::function<void()> tryGetTask(size_t id)
	{
		// Own queue
		{
			std::lock_guard<std::mutex> lk(queues_[id]->mx);
			for (int p = 0; p < static_cast<int>(TaskPriority::Count); ++p)
			{
				if (!queues_[id]->dq[p].empty())
				{
					auto t = std::move(queues_[id]->dq[p].front());
					queues_[id]->dq[p].pop_front();
					taskCount_.fetch_sub(1, std::memory_order_relaxed);
					return t;
				}
			}
		}
		// Steal from neighbours — use try_lock to skip busy victims rather than
		// blocking, which avoids an O(N) lock-acquire chain under burst steal load.
		for (size_t j = 1; j < numWorkers_; ++j)
		{
			size_t victim = (id + j) % numWorkers_;
			std::unique_lock<std::mutex> lk(queues_[victim]->mx, std::try_to_lock);
			if (!lk)
				continue;
			
			for (int p = 0; p < static_cast<int>(TaskPriority::Count); ++p)
			{
				if (!queues_[victim]->dq[p].empty())
				{
					auto t = std::move(queues_[victim]->dq[p].back());
					queues_[victim]->dq[p].pop_back();
					taskCount_.fetch_sub(1, std::memory_order_relaxed);
					return t;
				}
			}
		}
		return {};
	}

	void workerLoop(size_t id)
	{
		for (;;)
		{
			if (auto task = tryGetTask(id))
			{
				task();
				continue;
			}
			// Nothing available — sleep until work arrives or pool is stopping
			std::unique_lock<std::mutex> lk(sleepMux_);
			cv_.wait(lk, [this]
					 { return stop_.load(std::memory_order_relaxed) ||
							  taskCount_.load(std::memory_order_acquire) > 0; });
			if (stop_.load(std::memory_order_relaxed) &&
				taskCount_.load(std::memory_order_relaxed) == 0)
				return;
		}
	}

	size_t numWorkers_;
	std::atomic<bool> stop_;
	std::atomic<size_t> nextQueue_;
	std::atomic<size_t> taskCount_;
	std::vector<std::unique_ptr<LocalQueue>> queues_;
	std::vector<std::thread> workers_;
	std::mutex sleepMux_;
	std::condition_variable cv_;
};
