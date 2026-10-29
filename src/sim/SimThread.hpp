#pragma once
#include <thread>
#include <atomic>
#include "SimGraph.hpp"
#include "MessageQueue.hpp"
#include "SimMessages.hpp"
#include "../traffic/TrafficGraph.hpp"
#include "../debug/PerfStats.hpp"

/// @brief 経路計算サービススレッド
/// @details メッセージキューで RouteRequest を受け取り、Dijkstra を実行して
///   RouteResponse を返す。shared_mutex は不要。
class SimThread
{
public:
	// Main スレッドとはメッセージキュー越しにだけ通信し、探索グラフの可変状態はこのスレッドへ閉じ込める。
	SimThread() = default;
	~SimThread() { stop(); }

	/// @brief 旧ワーカを停止してから新しいスナップショットで開始する。所有スレッド専用。
	void start(std::shared_ptr<const SimGraph> graph);

	/// @brief スレッドを停止して join し、旧要求・応答を破棄する。所有スレッド専用。
	void stop();

	/// @brief リクエストを送信する（メインスレッドから呼ぶ）
	void pushRequest(SimRequest req)
	{
		if (m_running) { m_inbox.push(std::move(req)); }
	}

	/// @brief レスポンスを一括取得する（メインスレッドから呼ぶ）
	Array<SimResponse> drainResponses() { return m_outbox.drain(); }

private:
	void run();
	void handleRouteRequest(const RouteRequest& req);
	void handleNetworkUpdate(const NetworkUpdate& update);

	// メッセージキュー
	MessageQueue<SimRequest>  m_inbox;
	MessageQueue<SimResponse> m_outbox;

	// 経路探索グラフ（Sim スレッドのみがアクセス）
	TrafficGraph                    m_graph;
	std::shared_ptr<const SimGraph> m_simGraph;

	// スレッド制御
	std::thread       m_thread;
	std::atomic<bool> m_running = false;
};
