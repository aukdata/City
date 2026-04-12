#pragma once
#include "../time/GameClock.hpp"

/// @brief 黄色信号の持続時間 [ゲーム秒]
constexpr float kYellowDuration = 3.0f;

/// @brief 信号フェーズ（どの LaneConnection が青かを定義する）
struct SignalPhase
{
	float       duration;             ///< 青信号の持続時間 [ゲーム秒]（黄色時間を含まない）
	Array<int>  greenConnectionIds;   ///< このフェーズで青になる LaneConnection の ID リスト
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

	/// @brief 指定の LaneConnection が現在青かどうかを返す
	bool isGreen(int connectionId) const;

	/// @brief 指定の LaneConnection の期待待ち時間 [ゲーム秒] を返す
	float expectedWaitTime(int connectionId) const;

	int nodeId() const { return m_nodeId; }

	/// @brief 現在のフェーズインデックスを返す
	int currentPhaseIndex() const { return m_currentPhase; }

	/// @brief フェーズ数を返す
	int phaseCount() const { return static_cast<int>(m_phases.size()); }

	/// @brief 現在のフェーズで青になっている LaneConnection ID リストを返す
	const Array<int>& currentGreenConnections() const;

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
