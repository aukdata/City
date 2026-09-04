#include "ScenarioSystem.hpp"

namespace
{
	bool evaluateObjective(const ScenarioObjective& objective, const ScenarioContext& context)
	{
		switch (objective.metric)
		{
		case ScenarioMetric::PopulationAtLeast:
			return context.population >= objective.target;
		case ScenarioMetric::AverageSpeedAtLeast:
			return context.city.traffic.averageSpeedKmh >= objective.target;
		case ScenarioMetric::CongestedEdgesAtMost:
			return context.city.traffic.congestedEdgeCount <= objective.target;
		case ScenarioMetric::FundsAtLeast:
			return context.funds >= objective.target;
		case ScenarioMetric::BuiltRoadLengthAtLeast:
			return context.city.builtRoadLengthKm >= objective.target;
		}
		return false;
	}
}

void ScenarioSystem::start(ScenarioDefinition definition)
{
	m_active = std::move(definition);
	m_progress = {};
	if (m_active)
	{
		m_progress.objectiveCount = static_cast<int>(m_active->objectives.size());
		m_progress.objectiveCompleted.resize(m_active->objectives.size(), false);
	}
}

void ScenarioSystem::clear()
{
	m_active.reset();
	m_progress = {};
}

ScenarioProgress ScenarioSystem::evaluate(const ScenarioContext& context)
{
	if (!m_active) return m_progress;
	m_progress.completedObjectiveCount = 0;
	for (int i = 0; i < static_cast<int>(m_active->objectives.size()); ++i)
	{
		m_progress.objectiveCompleted[i] = evaluateObjective(m_active->objectives[i], context);
		if (m_progress.objectiveCompleted[i]) ++m_progress.completedObjectiveCount;
	}
	m_progress.completed = (m_progress.objectiveCount > 0
		&& m_progress.completedObjectiveCount == m_progress.objectiveCount);
	m_progress.failed = (!m_progress.completed
		&& m_active->deadlineMonthIndex >= 0
		&& context.monthIndex > m_active->deadlineMonthIndex);
	return m_progress;
}

ScenarioDefinition ScenarioSystem::MakeBypassScenario(int id, int64 startMonthIndex,
	double currentRoadLengthKm)
{
	ScenarioDefinition scenario;
	scenario.id = id;
	scenario.title = U"バイパス整備計画";
	scenario.description = U"新しい道路で混雑を分散し、街の移動速度を改善してください";
	scenario.deadlineMonthIndex = startMonthIndex + 24;
	scenario.objectives = {
		{ ScenarioMetric::BuiltRoadLengthAtLeast, currentRoadLengthKm + 2.0, U"道路を2km以上延伸" },
		{ ScenarioMetric::AverageSpeedAtLeast, 30.0, U"平均速度30km/h以上" },
		{ ScenarioMetric::CongestedEdgesAtMost, 3.0, U"渋滞区間を3区間以下" },
	};
	return scenario;
}
