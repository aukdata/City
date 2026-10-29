#pragma once
#include <Siv3D.hpp>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

/// @brief 独立したインデックスの CPU 処理を並列実行し、全ワーカを必ず合流させる。
namespace ParallelJobs
{
	/// @brief ワーカの例外を所有スレッドへ再送出する。task は異なる index で同時に呼ばれる。
	template <typename Task>
	void run(int count, Task&& task, unsigned concurrency = std::thread::hardware_concurrency())
	{
		if (count <= 0) { return; }
		const unsigned workers = Min(static_cast<unsigned>(count), Max(1u, concurrency));
		std::atomic<int> next{0};
		std::atomic<bool> failed{false};
		std::exception_ptr failure;
		std::mutex failureMutex;
		// jthread は起動途中の例外でも合流する。参照される状態より後に宣言する。
		Array<std::jthread> threads;
		threads.reserve(workers);
		for (unsigned worker = 0; worker < workers; ++worker)
		{
			threads.emplace_back([&]
			{
				try
				{
					while (!failed.load(std::memory_order_relaxed))
					{
						const int index = next.fetch_add(1, std::memory_order_relaxed);
						if (index >= count) { break; }
						task(index);
					}
				}
				catch (...)
				{
					std::lock_guard lock(failureMutex);
					if (!failure) { failure = std::current_exception(); }
					failed.store(true, std::memory_order_relaxed);
				}
			});
		}
		for (auto& thread : threads) { thread.join(); }
		if (failure) { std::rethrow_exception(failure); }
	}
}
