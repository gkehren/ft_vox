#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <future>
#include <condition_variable>
#include <functional>

class ThreadPool
{
public:
	ThreadPool(size_t numThreads) : stop(false)
	{
		for (size_t i = 0; i < numThreads; ++i)
		{
			workers.emplace_back([this]
								 {
					for (;;) {
						std::function<void()> task;
						{
							std::unique_lock<std::mutex> lock(queueMutex);
							condition.wait(lock, [this] { return stop || !tasks.empty(); });
							if (stop && tasks.empty())
								return;
							task = std::move(tasks.front());
							tasks.pop();
						}

						task();
					} });
		}
	}

	~ThreadPool()
	{
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			stop = true;
		}
		condition.notify_all();
		for (std::thread &worker : workers)
			worker.join();
	}

	template <class F>
	auto enqueue(F &&f) -> std::future<void>
	{
		auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
		std::future<void> res = task->get_future();
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			tasks.emplace([task]()
						  { (*task)(); });
		}
		condition.notify_one();
		return res;
	}

private:
	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks;
	std::mutex queueMutex;
	std::condition_variable condition;
	bool stop;
};
