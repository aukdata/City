#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../style/RoadStyleRegistry.hpp"

/// @brief 道路メッシュ・車線区画線の描画クラス
class RoadRenderer
{
public:
	/// @brief スタイル定義 TOML をロードする（ゲーム起動時に一度呼ぶ）
	/// @return ロード成功なら true
	bool loadStyle(FilePathView tomlPath);

	/// @brief 全エッジを描画する
	void render(const RoadNetwork& network, GameTime now, const World& world);

	/// @brief エッジのメッシュキャッシュを無効化する（道路変更時に呼ぶ）
	void markDirty(int edgeId);

private:
	// ---- キャッシュ構造 ----

	/// @brief 車線区画線の描画バッチ（色ごとにまとめる）
	struct LaneLineBatch
	{
		ColorF color;
		Mesh   mesh;
	};

	// ---- 描画サブルーチン ----

	/// @brief 単一エッジを描画する（路面メッシュ + 車線区画線）
	void drawEdge(const RoadEdge& edge, const CubicBezier& bezier, const World& world);

	/// @brief ベジェ曲線から道路路面の MeshData を生成する
	/// @note 路肩幅 (style.shoulderWidth) を両端に加えた幅でメッシュを生成する
	MeshData buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier,
	                       const RoadStyle& style, const World& world) const;

	/// @brief 車線区画線・センターラインのメッシュバッチを生成する
	/// @return 区画線バッチの配列（色ごとにまとめた Mesh）
	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const RoadStyle& style, const World& world) const;

	// ---- メンバ ----

	RoadStyleRegistry              m_styleRegistry;      ///< 道路種別 → スタイルのレジストリ
	HashTable<int, Mesh>           m_meshCache;          ///< エッジ ID → 路面メッシュ
	HashTable<int, Array<LaneLineBatch>> m_laneCache;    ///< エッジ ID → 車線区画線バッチ
};
