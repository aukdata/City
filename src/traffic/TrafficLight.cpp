#include "TrafficLight.hpp"

TrafficLight::TrafficLight(int nodeId, Array<SignalPhase> phases)
	: m_nodeId(nodeId)
	, m_phases(std::move(phases))
	, m_currentPhase(0)
	, m_phaseStart(0.0)
{
}

void TrafficLight::update(GameTime gameNow)
{
	if (m_phases.isEmpty()) return;

	const float duration = m_phases[m_currentPhase].duration;
	if (gameNow - m_phaseStart >= static_cast<double>(duration))
	{
		m_currentPhase = (m_currentPhase + 1) % static_cast<int>(m_phases.size());
		m_phaseStart   = gameNow;
	}
}

bool TrafficLight::isGreen(int fromEdgeId) const
{
	if (m_phases.isEmpty()) return true;  // フェーズなし = 常時青

	const auto& phase = m_phases[m_currentPhase];
	for (const int eid : phase.greenEdgeIds)
	{
		if (eid == fromEdgeId) return true;
	}
	return false;
}

float TrafficLight::expectedWaitTime(int fromEdgeId) const
{
	if (m_phases.isEmpty()) return 0.0f;
	if (isGreen(fromEdgeId)) return 0.0f;

	// 現在フェーズの持続時間を上限として返す（保守的な近似）
	return m_phases[m_currentPhase].duration;
}
