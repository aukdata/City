#pragma once
#include "../gameplay/CitySimulation.hpp"

/// @brief シナリオ目標の評価種別
enum class ScenarioMetric : uint8
{
	PopulationAtLeast,
	AverageSpeedAtLeast,
	CongestedEdgesAtMost,
	FundsAtLeast,
	BuiltRoadLengthAtLeast,
};

struct ScenarioObjective
{
	ScenarioMetric metric = ScenarioMetric::PopulationAtLeast;
	double target = 0.0;
	String description;
};

struct ScenarioDefinition
{
	int id = -1;
	String title;
	String description;
	int64 deadlineMonthIndex = -1;  ///< -1 は期限なし
	Array<ScenarioObjective> objectives;
};

struct ScenarioContext
{
	int population = 0;
	double funds = 0.0;
	int64 monthIndex = 0;
	CitySnapshot city;
};

struct ScenarioProgress
{
	bool completed = false;
	bool failed = false;
	int completedObjectiveCount = 0;
	int objectiveCount = 0;
	Array<bool> objectiveCompleted;
};

/// @brief 期限付き都市改善目標を評価するMVPシステム
class ScenarioSystem
{
public:
	void start(ScenarioDefinition definition);
	void clear();
	ScenarioProgress evaluate(const ScenarioContext& context);

	const Optional<ScenarioDefinition>& activeScenario() const { return m_active; }
	const ScenarioProgress& progress() const { return m_progress; }

	/// @brief バイパス建設を想定した基本シナリオを生成する
	static ScenarioDefinition MakeBypassScenario(int id, int64 startMonthIndex,
		double currentRoadLengthKm);

private:
	Optional<ScenarioDefinition> m_active;
	ScenarioProgress m_progress;
};

