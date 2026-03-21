#pragma once
#include <thread>
#include <shared_mutex>
#include <atomic>
#include "SimGraph.hpp"
#include "../time/GameClock.hpp"
#include "../traffic/TrafficManager.hpp"
#include "../railway/TrainManager.hpp"
#include "../event/EventSystem.hpp"

/// @brief シミュレーションスレッド
/// @details 車両・列車・イベントの更新をバックグラウンドで実行する。
///   メインスレッドは shared_lock で sim のデータを直接参照して描画する。
class SimThread
{
public:
	SimThread() = default;
	~SimThread() { stop(); }

	/// @brief シミュレーションスレッドを開始する
	/// @param traffic  車両シミュレーション（所有権を移譲）
	/// @param trainMgr 列車シミュレーション（所有権を移譲）
	/// @param events   イベントシステム（所有権を移譲）
	/// @param clock    ゲーム時計の初期状態
	/// @param graph    初期 SimGraph
	void start(TrafficManager&& traffic, TrainManager&& trainMgr,
	           EventSystem&& events, const GameClock& clock,
	           std::shared_ptr<const SimGraph> graph);

	/// @brief スレッドを停止して join する
	void stop();

	/// @brief ゲーム速度を設定する（メインスレッドから呼ぶ）
	void setSpeed(TimeSpeed speed) { m_speed.store(speed); }

	/// @brief SimGraph を差し替える（メインスレッドから呼ぶ）
	void swapGraph(std::shared_ptr<const SimGraph> newGraph);

	/// @brief sim データ読み取り用の shared_lock を取得する
	/// @details この lock を保持している間、sim スレッドはデータを更新しない。
	///   描画時に vehicles(), trains(), clock() を安全に参照できる。
	[[nodiscard]] std::shared_lock<std::shared_mutex> lockForRead()
	{
		return std::shared_lock<std::shared_mutex>{ m_dataMutex };
	}

	// ---- メインスレッドが shared_lock 保持中に参照可能 ----
	const Array<Vehicle>&      vehicles() const { return m_traffic.vehicles(); }
	const TrainManager&        trainManager() const { return m_trainMgr; }
	const GameClock&           clock() const { return m_clock; }
	Array<GameEvent>           popNotifications();

	/// @brief 車両をスポーンする（メインスレッドから呼ぶ、内部で lock 取得）
	void spawnVehicle(VehicleType type = VehicleType::PassengerCar);

	/// @brief ネットワーク変更を通知する（メインスレッドから呼ぶ）
	void notifyNetworkChanged(std::shared_ptr<const SimGraph> newGraph);

private:
	void run();

	// ---- sim 所有データ ----
	TrafficManager   m_traffic;
	TrainManager     m_trainMgr;
	EventSystem      m_events;
	GameClock        m_clock;

	// ---- スレッド間共有 ----
	std::shared_mutex                m_dataMutex;      ///< sim データの読み書きロック
	std::shared_ptr<const SimGraph>  m_graph;          ///< 現在の SimGraph
	std::mutex                       m_graphMutex;     ///< graph swap 用
	std::mutex                       m_notifyMutex;    ///< notification キュー用
	Array<GameEvent>                 m_pendingNotifications;

	// ---- スレッド制御 ----
	std::thread         m_thread;
	std::atomic<bool>   m_running = false;
	std::atomic<TimeSpeed> m_speed = TimeSpeed::x1;
};
