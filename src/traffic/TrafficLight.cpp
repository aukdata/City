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
	// 現在フェーズの青時間と黄時間をまとめて 1 周期とみなし、時間超過で次フェーズへ進める。
	if (m_phases.isEmpty())
	{
		return;
	}

	const float totalDuration = m_phases[m_currentPhase].duration + kYellowDuration;
	if (gameNow - m_phaseStart >= static_cast<double>(totalDuration))
	{
		m_currentPhase = (m_currentPhase + 1) % static_cast<int>(m_phases.size());
		m_phaseStart   = gameNow;
	}
}

bool TrafficLight::isGreen(int connectionId) const
{
	// 信号判定は現在フェーズの許可接続一覧だけを参照し、未設定信号は常時通行可として扱う。
	if (m_phases.isEmpty())
	{
		return true;
	}

	const auto& phase = m_phases[m_currentPhase];
	for (const int cid : phase.greenConnectionIds)
	{
		if (cid == connectionId)
		{
			return true;
		}
	}
	return false;
}

const Array<int>& TrafficLight::currentGreenConnections() const
{
	static const Array<int> empty;
	if (m_phases.isEmpty())
	{
		return empty;
	}
	return m_phases[m_currentPhase].greenConnectionIds;
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
	return m_phases[m_currentPhase].duration + kYellowDuration;
}

float TrafficLight::expectedWaitTime(int connectionId) const
{
	// 待ち時間は厳密予測ではなく、今が赤なら現在フェーズ残り相当を返す保守的近似に留める。
	if (m_phases.isEmpty())
	{
		return 0.0f;
	}
	if (isGreen(connectionId))
	{
		return 0.0f;
	}

	// 現在フェーズの持続時間を上限として返す（保守的な近似）
	return m_phases[m_currentPhase].duration + kYellowDuration;
}
