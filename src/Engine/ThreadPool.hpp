#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class ThreadPool
{
	public:
		ThreadPool(size_t numThreads) {
			start(numThreads);
		}

		~ThreadPool() {
			stop();
		}

		template<class T>
		void enqueue(T task) {
			{
				std::unique_lock<std::mutex> lock(mEventMutex);
				mTasks.emplace(std::make_shared<Task<T>>(std::move(task)));
			}
			mEventVar.notify_one();
		}

	private:
		struct TaskBase {
			virtual ~TaskBase() {}
			virtual void execute() = 0;
		};

		template<class T>
		struct Task : TaskBase {
			Task(T func) : mFunc(std::move(func)) {}
			void execute() override { mFunc(); }

			T mFunc;
		};

		void start(size_t numThreads) {
			for (auto i = 0u; i < numThreads; ++i) {
				mThreads.emplace_back([=] {
					while (true) {
						std::shared_ptr<TaskBase> task = nullptr;
						{
							std::unique_lock<std::mutex> lock(mEventMutex);
							mEventVar.wait(lock, [=] { return mStopped || !mTasks.empty(); });

							if (mStopped && mTasks.empty())
								break;

							task = std::move(mTasks.front());
							mTasks.pop();
						}

						task->execute();
					}
				});
			}
		}

		void stop() noexcept {
			{
				std::unique_lock<std::mutex> lock(mEventMutex);
				mStopped = true;
			}
			mEventVar.notify_all();

			for (auto& thread : mThreads)
				thread.join();
		}

		std::vector<std::thread> mThreads;
		std::queue<std::shared_ptr<TaskBase>> mTasks;
		std::mutex mEventMutex;
		std::condition_variable mEventVar;
		bool mStopped = false;
};
