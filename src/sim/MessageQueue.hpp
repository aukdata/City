#pragma once
#include <Siv3D.hpp>
#include <mutex>
#include <condition_variable>

/// @brief スレッドセーフなメッセージキュー
/// @details push/drain のみ。ロック保持時間はポインタ swap の数 us。
template <typename T>
class MessageQueue
{
public:
	/// @brief メッセージを追加する
	void push(T msg)
	{
		{
			std::lock_guard lock(m_mutex);
			m_queue << std::move(msg);
		}
		m_cv.notify_one();
	}

	/// @brief 全メッセージを一括取り出し（O(1) swap）
	Array<T> drain()
	{
		std::lock_guard lock(m_mutex);
		Array<T> result = std::move(m_queue);
		m_queue.clear();
		return result;
	}

	/// @brief メッセージが届くまで待機する（タイムアウト付き）
	/// @return true: メッセージあり / false: タイムアウト
	bool waitFor(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		return m_cv.wait_for(lock, timeout, [&] { return !m_queue.empty(); });
	}

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	Array<T> m_queue;
};
