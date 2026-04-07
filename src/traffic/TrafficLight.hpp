#pragma once
#include "../time/GameClock.hpp"

/// @brief 信号フェーズ（どの進入エッジが青かを定義する）
struct SignalPhase
{
	float       duration;         ///< フェーズ持続時間 [ゲーム秒]
	Array<int>  greenEdgeIds;     ///< このフェーズで青になる進入エッジ ID リスト
};

/// @brief 信号機（RoadNode に紐づく）
class TrafficLight
{
public:
	/// @brief コンストラクタ
	/// @param nodeId    対象 RoadNode の ID
	/// @param phases    信号フェーズのリスト（空なら常時青）
	explicit TrafficLight(int nodeId, Array<SignalPhase> phases);

	/// @brief ゲーム時刻に基づいてフェーズを更新する
	void update(GameTime gameNow);

	/// @brief 指定の進入エッジが現在青かどうかを返す
	bool isGreen(int fromEdgeId) const;

	/// @brief 指定の進入エッジの期待待ち時間 [ゲーム秒] を返す
	float expectedWaitTime(int fromEdgeId) const;

	int nodeId() const { return m_nodeId; }

	/// @brief 現在のフェーズインデックスを返す
	int currentPhaseIndex() const { return m_currentPhase; }

	/// @brief フェーズ数を返す
	int phaseCount() const { return static_cast<int>(m_phases.size()); }

	/// @brief 現在のフェーズで青になっているエッジ ID リストを返す
	const Array<int>& currentGreenEdges() const;

	/// @brief 現在のフェーズの経過時間 [ゲーム秒] を返す
	float phaseElapsed(GameTime gameNow) const;

	/// @brief 現在のフェーズの持続時間を返す
	float currentPhaseDuration() const;

private:
	int                m_nodeId;
	Array<SignalPhase> m_phases;
	int                m_currentPhase  = 0;
	GameTime           m_phaseStart    = 0.0;
};
