#pragma once
#include <Siv3D.hpp>
#include <array>

/// @brief 1フレーム/1tickの計測値
struct MainFrameStats
{
	// 描画内訳を固定フィールドで持ち、毎フレーム同じ順序で比較・表示できるようにする。
	double lockWait = 0;
	double sky      = 0;
	double terrain  = 0;
	double road     = 0;
	double zone     = 0;
	double vehicle  = 0;
	double train    = 0;
	double debugUI  = 0;

	double total() const
	{
		return lockWait + sky + terrain + road + zone + vehicle + train + debugUI;
	}
};

struct SimTickStats
{
	// Sim 側も同様に主要処理ごとの時間を分離し、経路探索と交通更新の偏りを追いやすくする。
	double graph      = 0;
	double reroute    = 0;
	double idm        = 0;
	double laneChange = 0;
	double signal     = 0;
	double other      = 0;

	double total() const
	{
		return graph + reroute + idm + laneChange + signal + other;
	}
};

/// @brief 固定長リングバッファ
template <typename T, size_t N>
class RingBuffer
{
public:
	// 固定長の履歴を使い回し、計測履歴の収集で動的確保を増やさない。
	void push(const T& value)
	{
		m_buf[m_head] = value;
		m_head = (m_head + 1) % N;
		if (m_count < N) ++m_count;
	}

	/// @brief i=0 が最も古い、i=count()-1 が最新
	const T& operator[](size_t i) const
	{
		return m_buf[(m_head + N - m_count + i) % N];
	}

	size_t count() const { return m_count; }
	size_t capacity() const { return N; }

	const T& latest() const
	{
		return m_buf[(m_head + N - 1) % N];
	}

private:
	std::array<T, N> m_buf{};
	size_t m_head  = 0;
	size_t m_count = 0;
};

constexpr size_t kPerfHistorySize = 120;

using MainPerfHistory = RingBuffer<MainFrameStats, kPerfHistorySize>;
using SimPerfHistory  = RingBuffer<SimTickStats, kPerfHistorySize>;
