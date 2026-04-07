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
	if (m_phases.isEmpty())
	{
		return;
	}

	const float duration = m_phases[m_currentPhase].duration;
	if (gameNow - m_phaseStart >= static_cast<double>(duration))
	{
		m_currentPhase = (m_currentPhase + 1) % static_cast<int>(m_phases.size());
		m_phaseStart   = gameNow;
	}
}

bool TrafficLight::isGreen(int fromEdgeId) const
{
	if (m_phases.isEmpty())
	{
		return true;
	}

	const auto& phase = m_phases[m_currentPhase];
	for (const int eid : phase.greenEdgeIds)
	{
		if (eid == fromEdgeId)
		{
			return true;
		}
	}
	return false;
}

const Array<int>& TrafficLight::currentGreenEdges() const
{
	static const Array<int> empty;
	if (m_phases.isEmpty())
	{
		return empty;
	}
	return m_phases[m_currentPhase].greenEdgeIds;
}

float TrafficLight::phaseElapsed(GameTime gameNow) const
{
	return static_cast<float>(gameNow - m_phaseStart);
}

float TrafficLight::currentPhaseDuration() const
{
	if (m_phases.isEmpty())
	{
		return 0.0f;
	}
	return m_phases[m_currentPhase].duration;
}

float TrafficLight::expectedWaitTime(int fromEdgeId) const
{
	if (m_phases.isEmpty())
	{
		return 0.0f;
	}
	if (isGreen(fromEdgeId))
	{
		return 0.0f;
	}

	// 現在フェーズの持続時間を上限として返す（保守的な近似）
	return m_phases[m_currentPhase].duration;
}
