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
	SimThread() = default;
	~SimThread() { stop(); }

	/// @brief スレッドを開始する
	void start(std::shared_ptr<const SimGraph> graph);

	/// @brief スレッドを停止して join する
	void stop();

	/// @brief リクエストを送信する（メインスレッドから呼ぶ）
	void pushRequest(SimRequest req) { m_inbox.push(std::move(req)); }

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
