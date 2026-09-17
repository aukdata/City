#pragma once
#include "Train.hpp"

class TrainNetwork;
struct TrainSchedule;
struct StopEntry;

/// @brief ダイヤに基づく編成生成・駅停車・単線経路の排他予約。
class TrainManager
{
public:
	/// @brief 線路網を参照し、列車と占有状態を初期化する。
	void init(TrainNetwork* network);
	/// @brief シミュレーション秒で更新する。停止中は生成も行わない。
	void update(double dt, GameTime gameNow);
	const Array<Train>& trains() const { return m_trains; }
	/// @brief 手動で列車を追加する。
	void addTrain(Train train);

private:
	static constexpr double kMaximumStep = 0.1;  ///< 積分の最大刻み [秒]
	static constexpr float kMaxAccel = 1.0f;     ///< 最大加速度 [m/s²]
	static constexpr float kMaxDecel = 1.0f;     ///< 最大減速度 [m/s²]
	static constexpr float kArrivalTolerance = 0.05f;
	static constexpr float kLookAheadDistance = 1000.0f;
	static constexpr float kDepartureClearance = 2.0f;

	TrainNetwork* m_network = nullptr;
	Array<Train> m_trains;
	int m_nextId = 0;

	void updateTrain(Train& train, double dt);
	void advanceTrain(Train& train, double dt);
	void updatePosition(Train& train);
	/// @brief 終着・経路喪失で便を終了し、予約をまとめて解放する。
	void finishService(Train& train);
	/// @brief 往路・復路で共通の停車駅選択。
	Optional<StopEntry> nextStop(const Train& train) const;
	float targetSpeed(const Train& train) const;

	void spawnScheduledTrains(GameTime gameNow);
	Array<int> buildServiceRoute(const TrainSchedule& schedule) const;
	bool canReserveRoute(const Array<int>& route, TrainType type) const;
	Train makeScheduledTrain(const TrainSchedule& schedule, Array<int> route, GameTime now) const;
};
