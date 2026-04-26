#pragma once
#include "Train.hpp"
#include "TrainNetwork.hpp"

/// @brief 列車管理クラス
/// @details ダイヤに基づく生成・IDM走行・閉塞制御
class TrainManager
{
public:
	// TrainNetwork を参照しながら、列車の生成・進行・閉塞待ちを一括で管理する。
	/// @brief 初期化
	void init(TrainNetwork* network);

	/// @brief フレーム更新
	void update(double dt, GameTime gameNow);

	/// @brief 列車一覧を返す（読み取り専用）
	const Array<Train>& trains() const { return m_trains; }

	/// @brief 手動で列車を追加する
	void addTrain(Train train);

private:
	static constexpr float kMaxAccel  = 1.0f;  ///< 最大加速度 [m/s²]
	static constexpr float kMaxDecel  = 3.0f;  ///< 最大減速度 [m/s²]
	static constexpr float kBrakeZone = 150.0f; ///< 停止準備距離 [m]

	TrainNetwork*  m_network = nullptr;
	Array<Train>   m_trains;
	int            m_nextId  = 0;

	void updateTrain(Train& t, double dt, GameTime gameNow);
	void advanceTrain(Train& t, double dt, GameTime gameNow);

	/// @brief ダイヤに従い列車を生成する
	void spawnScheduledTrains(GameTime gameNow);

	/// @brief 列車の目標速度を取得する（信号・速度制限考慮）
	float targetSpeed(const Train& t) const;
};
