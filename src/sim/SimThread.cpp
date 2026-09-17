#include "SimThread.hpp"
#include <chrono>

void SimThread::start(std::shared_ptr<const SimGraph> graph)
{
	// スレッド開始前に最初の SimGraph から探索グラフを構築し、以後は専用スレッド内で更新する。
	m_simGraph = std::move(graph);

	// 初回グラフ構築
	if (m_simGraph)
	{
		HashTable<int, TrafficLight> emptyLights;
		m_graph.rebuild(*m_simGraph, 0.0, emptyLights);
	}

	m_running = true;
	m_thread  = std::thread{ &SimThread::run, this };
}

void SimThread::stop()
{
	// 停止時は待機中 inbox を起こしてスレッド終了まで合流し、取り残しを防ぐ。
	m_running = false;
	// inbox に空メッセージを送って waitFor を起こす
	m_inbox.push(NetworkUpdate{ nullptr });
	if (m_thread.joinable())
		m_thread.join();
}

void SimThread::run()
{
	// SimThread は到着したリクエスト群をまとめて処理し、探索結果と計測値を outbox へ返す。
	while (m_running)
	{
		// リクエストが届くまで待機（10ms タイムアウト）
		m_inbox.waitFor(std::chrono::milliseconds(10));

		auto requests = m_inbox.drain();
		if (requests.isEmpty()) continue;

		using Clock = std::chrono::steady_clock;
		const auto tStart = Clock::now();

		SimTickStats stats{};

		for (auto& req : requests)
		{
			std::visit([&](auto& r)
			{
				using T = std::decay_t<decltype(r)>;
				if constexpr (std::is_same_v<T, RouteRequest>)
					handleRouteRequest(r);
				else if constexpr (std::is_same_v<T, NetworkUpdate>)
					handleNetworkUpdate(r);
			}, req);
		}

		const double totalMs = std::chrono::duration<double, std::milli>(
			Clock::now() - tStart).count();
		stats.reroute = totalMs;
		m_outbox.push(PerfUpdate{ stats });
	}
}

void SimThread::handleRouteRequest(const RouteRequest& req)
{
	// 経路探索要求では startLane を入口ノードへ解決し、見つからなければ同一エッジ内で代替車線を探す。
	if (!m_simGraph) return;

	// 開始ノードを探す
	int startNode = m_graph.entryNodeId(req.startEdge, req.startLane);
	if (startNode == -1)
	{
		const auto* e = m_simGraph->getEdge(req.startEdge);
		if (e)
		{
			for (int i = 0; i < static_cast<int>(e->lanes.size()); ++i)
			{
				startNode = m_graph.entryNodeId(req.startEdge, i);
				if (startNode != -1) break;
			}
		}
	}

	RouteResponse resp;
	resp.vehicleId = req.vehicleId;

	if (startNode == -1)
	{
		resp.found = false;
		m_outbox.push(std::move(resp));
		return;
	}

	// 探索結果は lane node 列から RouteWaypoint 列へ落とし直し、Main 側がそのまま消化できる形で返す。
	const PathResult result = m_graph.dijkstra(startNode, req.goalEdge, req.goalLane);

	resp.found = result.found;

	if (result.found)
	{
		// LaneNode ID 列 → RouteWaypoint 列に変換
		int prevEdge = req.startEdge;
		for (const int nodeId : result.nodeIds)
		{
			const LaneNode* ln = m_graph.getLaneNode(nodeId);
			if (!ln) continue;
			if (ln->edgeId == prevEdge) continue;

			const auto* edge = m_simGraph->getEdge(ln->edgeId);
			const float length = edge ? edge->length : 100.0f;
			const float speedMs = edge ? Max(1.0f, edge->speedLimit / 3.6f) : 10.0f;

			RouteWaypoint wp;
			wp.edgeId          = ln->edgeId;
			wp.laneIndex       = ln->laneIndex;
			wp.entryArcPos     = ln->arcPos;
			wp.edgeLength      = length;
			wp.estimatedTimeSec = length / speedMs;
			resp.waypoints << wp;

			prevEdge = ln->edgeId;
		}
	}

	m_outbox.push(std::move(resp));
}

void SimThread::handleNetworkUpdate(const NetworkUpdate& update)
{
	// ネットワーク差し替え時は SimGraph を丸ごと更新し、探索グラフもその場で再構築する。
	if (!update.graph) return;
	m_simGraph = update.graph;
	if (update.kind == NetworkChangeKind::MovedIntersectionNode)
	{
		m_graph.updateMovedIntersectionNode(*m_simGraph, update.dirtyNodeIds);
	}
	else
	{
		HashTable<int, TrafficLight> emptyLights;
		m_graph.rebuild(*m_simGraph, 0.0, emptyLights);
	}
}
