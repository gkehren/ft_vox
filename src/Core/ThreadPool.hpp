#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <future>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

// Add above ThreadPool class
enum class TaskPriority { High = 0, Normal = 1, Low = 2, Count = 3 };

class ThreadPool;
// Which pool (if any) the calling thread is a worker of. Lets a running
// task enqueue follow-up work into its OWN pool while that pool drains at
// shutdown (the biome-map parent/children contract), while external
// producers are refused once stop is set (review M3).
inline thread_local ThreadPool *t_workerPool = nullptr;

// L: Work-stealing thread pool.
// Each worker owns a local deque. 'submit'/'enqueue' round-robin tasks across
// workers. Idle workers steal from the back of a neighbour's deque, which
// keeps recently-enqueued (hot) tasks in the owner thread while spreading
// older (cold) work to stealers — reducing single-queue lock contention under
// burst load.
//
// Shutdown contract (review M3): the destructor sets stop_, wakes every
// sleeper and waits for in-flight submissions to finish publishing BEFORE
// the workers are joined and the queues destroyed. submit()/enqueue() on a
// stopped (or stopping) pool throw std::runtime_error instead of pushing a
// task no worker will ever run (a future that never becomes ready would
// hang its waiter). Hosts must still stop feeding the pool before destroying
// it; the class now defends itself against the residual race instead of
// relying on host discipline alone.
class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads)
		: numWorkers_(numThreads), stop_(false), nextQueue_(0), taskCount_(0)
	{
		if (numThreads == 0)
			throw std::invalid_argument("ThreadPool requires at least one worker");
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
		// Stop gate (review M3): a submission that passed the stop check
		// before the store above still owns a queue slot; wait until every
		// in-flight submission has published (and decremented the gate)
		// before the jthreads join and the queues are destroyed. Workers
		// also keep draining queued tasks while the gate is held, so a
		// racing producer's task is executed, never orphaned.
		while (enqueueGate_.load() != 0)
			std::this_thread::yield();
	}

	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	/// Fire-and-forget submission — the hot path for generation/mesh/light
	/// jobs whose results flow through completion queues, not futures.
	/// One heap allocation per job (the type-erased callable), no
	/// packaged_task / shared_ptr / std::function overhead (review M4).
	template <class F>
	void submit(TaskPriority priority, F &&f)
	{
		EnqueueGate gate(*this);
		publish(priority, Task{std::forward<F>(f)});
	}

	template <class F>
	void submit(F &&f)
	{
		submit(TaskPriority::Normal, std::forward<F>(f));
	}

	/// Future-based submission, kept for callers that consume the result
	/// (tests, tools). packaged_task is only paid for on this path
	/// (review M4); prefer submit() when the future would be ignored.
	template <class F>
	auto enqueue(TaskPriority priority, F &&f) -> std::future<void>
	{
		EnqueueGate gate(*this);
		std::packaged_task<void()> task(std::forward<F>(f));
		std::future<void> res = task.get_future();
		publish(priority, Task{std::move(task)});
		return res;
	}

	size_t workerCount() const { return numWorkers_; }

	// Overload for backward compatibility (defaults to Normal)
	template <class F>
	auto enqueue(F &&f) -> std::future<void>
	{
		return enqueue(TaskPriority::Normal, std::forward<F>(f));
	}

private:
	// Move-only type-erased callable (review M4): a tiny hand-rolled vtable
	// instead of std::function, so a job costs exactly one allocation and
	// no atomic refcount traffic.
	class Task
	{
		struct Concept
		{
			virtual ~Concept() = default;
			virtual void call() = 0;
		};
		template <class F>
		struct Model final : Concept
		{
			F f;
			explicit Model(F &&fn) : f(std::move(fn)) {}
			void call() override { f(); }
		};
		std::unique_ptr<Concept> ptr;

	public:
		Task() = default;
		template <class F>
		explicit Task(F &&f) : ptr(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}
		Task(Task &&) = default;
		Task &operator=(Task &&) = default;
		explicit operator bool() const { return ptr != nullptr; }
		void operator()() { ptr->call(); }
	};

	struct LocalQueue
	{
		std::deque<Task> dq[static_cast<int>(TaskPriority::Count)];
		std::mutex mx;
	};

	// Stop-check + in-flight gate (review M3). Double-check pattern: the
	// gate increment is sequenced before the second stop load, and the
	// destructor spins on the gate after storing stop — with seq_cst
	// ordering a submission either observes stop and throws, or publishes
	// fully before the destructor proceeds (workers keep draining while
	// the gate is held, so the task always runs). No mutex on the
	// steady-state path (review M4: the old empty-lock scope serialized
	// every producer).
	//
	// Drain exception: a worker of THIS pool may enqueue follow-up tasks
	// while the pool drains at shutdown (parent job -> child tiles); the
	// workers only exit once the queues AND the gate are empty, so those
	// children are guaranteed to run. External producers are refused.
	bool enqueueAllowed() const
	{
		return !stop_.load() || t_workerPool == this;
	}
	void enterEnqueue()
	{
		if (!enqueueAllowed())
			throw std::runtime_error("enqueue on stopped ThreadPool");
		enqueueGate_.fetch_add(1);
		if (!enqueueAllowed())
		{
			enqueueGate_.fetch_sub(1);
			notifyIfStopping();
			throw std::runtime_error("enqueue on stopped ThreadPool");
		}
	}
	// A refused submission decremented the gate without publishing: wake
	// any worker whose exit predicate was waiting on that gate.
	void notifyIfStopping()
	{
		if (stop_.load() && sleepers_.load() > 0)
		{
			std::lock_guard<std::mutex> lk(sleepMux_);
			cv_.notify_all();
		}
	}

	struct EnqueueGate
	{
		ThreadPool &pool;
		explicit EnqueueGate(ThreadPool &p) : pool(p) { pool.enterEnqueue(); }
		~EnqueueGate()
		{
			pool.enqueueGate_.fetch_sub(1);
			pool.notifyIfStopping();
		}
		EnqueueGate(const EnqueueGate &) = delete;
		EnqueueGate &operator=(const EnqueueGate &) = delete;
	};

	void publish(TaskPriority priority, Task &&task)
	{
		// Distribute round-robin across worker queues
		const size_t qi = nextQueue_.fetch_add(1) % numWorkers_;
		{
			std::lock_guard<std::mutex> lk(queues_[qi]->mx);
			queues_[qi]->dq[static_cast<int>(priority)].emplace_front(std::move(task)); // LIFO push
			taskCount_.fetch_add(1);
		}
		// Wake a worker only when one is actually sleeping (review M4).
		// Lost-wakeup safety: sleepers_ is incremented under sleepMux_
		// BEFORE the predicate wait, and every atomic here is seq_cst —
		// if this load sees 0, the sleeper's later predicate check is
		// ordered after the taskCount_ increment above and cannot miss it.
		if (sleepers_.load() > 0)
		{
			std::lock_guard<std::mutex> lk(sleepMux_);
			cv_.notify_one();
		}
	}

	// Try to obtain a task: own queue front (LIFO — cache-warm), then steal from
	// another worker's back (FIFO — oldest items first, minimises latency).
	Task tryGetTask(size_t id)
	{
		// Search EVERY queue for High before Normal, and Normal before Low.
		// Local Low map tiles must not hide queued High/Normal chunk work.
		for (int p = 0; p < static_cast<int>(TaskPriority::Count); ++p)
		{
			for (size_t j = 0; j < numWorkers_; ++j)
			{
				const size_t victim = (id + j) % numWorkers_;
				std::lock_guard<std::mutex> lk(queues_[victim]->mx);
				auto &queue = queues_[victim]->dq[p];
				if (queue.empty())
					continue;
				auto task = std::move(j == 0 ? queue.front() : queue.back());
				if (j == 0)
					queue.pop_front();
				else
					queue.pop_back();
				taskCount_.fetch_sub(1);
				return task;
			}
		}
		return {};
	}

	void workerLoop(size_t id)
	{
		t_workerPool = this;
		for (;;)
		{
			if (auto task = tryGetTask(id))
			{
				task();
				continue;
			}
			// Nothing available — sleep until work arrives or the pool has
			// stopped AND no submission is still in flight (the gate: a
			// racing producer may yet publish a task we must drain).
			std::unique_lock<std::mutex> lk(sleepMux_);
			sleepers_.fetch_add(1);
			cv_.wait(lk, [this]
					 { return taskCount_.load() > 0 ||
							  (stop_.load() && enqueueGate_.load() == 0); });
			sleepers_.fetch_sub(1);
			if (stop_.load() && taskCount_.load() == 0 && enqueueGate_.load() == 0)
			{
				t_workerPool = nullptr;
				return;
			}
		}
	}

	// Cache-line isolation of the hot atomics (review §4.2): stop_,
	// nextQueue_, taskCount_ and the gate are written on every
	// enqueue/steal; keeping them on separate lines avoids false sharing
	// between producers and workers. 64 covers x86-64 and Apple silicon.
	static constexpr size_t kCacheLine = 64;

	size_t numWorkers_;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // the cache-line padding below is INTENTIONAL
#endif
	alignas(kCacheLine) std::atomic<bool> stop_;
	alignas(kCacheLine) std::atomic<size_t> nextQueue_;
	alignas(kCacheLine) std::atomic<size_t> taskCount_;
	alignas(kCacheLine) std::atomic<size_t> sleepers_{0};
	alignas(kCacheLine) std::atomic<size_t> enqueueGate_{0};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
	std::vector<std::unique_ptr<LocalQueue>> queues_;
	std::mutex sleepMux_;
	std::condition_variable cv_;
	// Declared last: joined before the queues/synchronization members above
	// are destroyed.
	std::vector<std::jthread> workers_;
};
