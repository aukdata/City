#pragma once
#include "../road/RoadNetwork.hpp"
#include "../traffic/TrafficManager.hpp"
#include "../world/World.hpp"
#include "../ui/Camera.hpp"

/// @brief デバッグオーバーレイ描画クラス
/// @details F3 でデバッグモード ON/OFF、F3+X で個別機能トグル
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

	bool isDebugMode() const { return m_debugMode; }

private:
	bool m_debugMode     = false;
	bool m_showNetwork   = false;  ///< F3+N: ネットワーク可視化
	bool m_showChunks    = false;  ///< F3+C: チャンク境界
	bool m_showVehicles  = false;  ///< F3+V: 車両デバッグ情報
	bool m_showGrid      = false;  ///< F3+G: ワールド座標グリッド
	bool m_showDetailHUD = false;  ///< F3+H: HUD 詳細
	bool   m_showHelp      = false;  ///< F3+/: ヘルプパネル
	double m_helpOpenTime  = 0.0;    ///< ヘルプを開いた Scene::Time()

	Font m_font{ FontMethod::MSDF, 16 };

	void renderNetwork(const RoadNetwork& network, const GameCamera& camera);
	void renderChunks(const World& world, const GameCamera& camera);
	void renderVehicleInfo(const Array<Vehicle>& vehicles, const GameCamera& camera);
	void renderGrid(const GameCamera& camera);
	void renderDetailHUD(const RoadNetwork& network,
	                     const World& world,
	                     const GameCamera& camera,
	                     const Array<Vehicle>& vehicles);
	void renderLog();
	void renderHelp();
};
