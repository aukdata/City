#include "SimThread.hpp"
#include <chrono>

void SimThread::start(
	TrafficManager&& traffic, TrainManager&& trainMgr,
	EventSystem&& events, const GameClock& clock,
	std::shared_ptr<const SimGraph> graph)
{
	m_traffic  = std::move(traffic);
	m_trainMgr = std::move(trainMgr);
	m_events   = std::move(events);
	m_clock    = clock;
	m_graph    = std::move(graph);

	// TrafficManager の SimGraph を共有ポインタに差し替え（再 rebuild はしない）
	m_traffic.setSimGraph(m_graph);

	m_running  = true;
	m_thread   = std::thread{ &SimThread::run, this };
}

void SimThread::stop()
{
	m_running = false;
	if (m_thread.joinable())
		m_thread.join();
}

void SimThread::swapGraph(std::shared_ptr<const SimGraph> newGraph)
{
	std::lock_guard lock(m_graphMutex);
	m_graph = std::move(newGraph);
}

Array<GameEvent> SimThread::popNotifications()
{
	std::lock_guard lock(m_notifyMutex);
	Array<GameEvent> result = std::move(m_pendingNotifications);
	m_pendingNotifications.clear();
	return result;
}

void SimThread::spawnVehicle(VehicleType type)
{
	std::unique_lock lock(m_dataMutex);
	m_traffic.spawnVehicle(type);
}

void SimThread::notifyNetworkChanged(std::shared_ptr<const SimGraph> newGraph)
{
	// SimGraph を swap
	{
		std::lock_guard lock(m_graphMutex);
		m_graph = newGraph;
	}
	// TrafficManager に通知（次の update で反映される）
	{
		std::unique_lock lock(m_dataMutex);
		m_traffic.setSimGraph(std::move(newGraph));
		m_traffic.markNetworkDirty();
	}
}

void SimThread::run()
{
	using Clock = std::chrono::steady_clock;
	constexpr auto kTickInterval = std::chrono::microseconds(16667); // ~60Hz

	auto lastTick = Clock::now();

	while (m_running)
	{
		const auto now = Clock::now();
		const double realDt = std::chrono::duration<double>(now - lastTick).count();
		lastTick = now;

		const TimeSpeed speed = m_speed.load();

		// ポーズ中はスリープのみ
		if (speed == TimeSpeed::Paused)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		const double physicsDt = realDt * static_cast<double>(static_cast<int>(speed)) / 60.0;

		// ---- sim データ更新 (unique_lock) ----
		{
			std::unique_lock lock(m_dataMutex);
			m_clock.advance(realDt);
			m_traffic.update(physicsDt, m_clock.now);
			m_trainMgr.update(physicsDt, m_clock.now);
			m_events.update(m_clock.now, m_clock.month, realDt);
		}

		// ---- 通知キュー更新 ----
		{
			const auto notifications = m_events.popNewNotifications();
			if (!notifications.isEmpty())
			{
				std::lock_guard lock(m_notifyMutex);
				for (const auto& n : notifications)
					m_pendingNotifications << n;
			}
		}

		// ---- フレームレート制御 ----
		const auto elapsed = Clock::now() - now;
		if (elapsed < kTickInterval)
			std::this_thread::sleep_for(kTickInterval - elapsed);
	}
}
