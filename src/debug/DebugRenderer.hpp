#pragma once
#include "../road/RoadNetwork.hpp"
#include "../traffic/Vehicle.hpp"
#include "../world/World.hpp"
#include "../ui/Camera.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "PerfStats.hpp"

/// @brief デバッグオーバーレイ描画クラス
/// @details F3+X で各機能を個別に ON/OFF する
class DebugRenderer
{
public:
	/// @brief キー入力を処理する（毎フレーム update 末尾で呼ぶ）
	void handleInput();

	/// @brief デバッグオーバーレイを描画する（毎フレーム render 末尾で呼ぶ）
	void render(const RoadNetwork& network,
	            const Array<Vehicle>& vehicles,
	            const World& world,
	            const GameCamera& camera);

	/// @brief フレーム時間プロファイラ HUD を描画する
	void renderProfiler(double total, double logic, double sky, double terrain,
	                    double road, double zone, double vehicle, double train,
	                    double debug, double ui,
	                    const RoadNetwork& network);

	/// @brief パフォーマンスグラフを描画する
	void renderPerfGraph(const MainPerfHistory& mainHistory,
	                     const SimPerfHistory& simHistory);

	bool isDebugMode() const { return m_showProfiler || m_showNetwork || m_showChunks
		|| m_showVehicles || m_showGrid || m_showDetailHUD || m_showBiomes || m_showPerfGraph; }

private:
	bool m_showProfiler  = false;  ///< F3+D: 数値プロファイラ
	bool m_showNetwork   = false;  ///< F3+N: ネットワーク可視化
	bool m_showChunks    = false;  ///< F3+C: チャンク境界
	bool m_showVehicles  = false;  ///< F3+V: 車両デバッグ情報
	bool m_showGrid      = false;  ///< F3+G: ワールド座標グリッド
	bool m_showDetailHUD = false;  ///< F3+H: HUD 詳細
	bool m_showBiomes    = false;  ///< F3+B: バイオーム表示
	bool m_showPerfGraph = false;  ///< F3+P: パフォーマンスグラフ
	bool   m_showHelp      = false;  ///< F3+/: ヘルプパネル
	double m_helpOpenTime  = 0.0;    ///< ヘルプを開いた Scene::Time()

	Font m_font = FontAsset(Asset::Small16);

	void renderNetwork(const RoadNetwork& network, const GameCamera& camera);
	void renderChunks(const World& world, const GameCamera& camera);
	void renderVehicleInfo(const Array<Vehicle>& vehicles, const GameCamera& camera);
	void renderGrid(const GameCamera& camera);
	void renderDetailHUD(const RoadNetwork& network,
	                     const World& world,
	                     const GameCamera& camera,
	                     const Array<Vehicle>& vehicles);
	void renderBiomes(const World& world, const GameCamera& camera);
	void renderBiomeLegend();
	void renderLog();
	void renderHelp();

	/// @brief 積み上げ棒グラフを描画するヘルパー
	template <typename TStats, typename FSegments>
	void drawStackedBarGraph(Vec2 origin, double width, double height,
	                         const RingBuffer<TStats, kPerfHistorySize>& history,
	                         StringView title, FSegments getSegments);

	/// @brief 直近の計測値を Console に出力する
	void dumpPerfToConsole(const MainPerfHistory& mainHistory,
	                       const SimPerfHistory& simHistory);
};
