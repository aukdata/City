#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 勾配・曲率を制約とし、延長と土工・構造物費を比較する道路線形探索。
namespace RoadAlignment
{
	struct Result
	{
		Array<CubicBezier> curves;
		double cost = 0;
		int expanded = 0;
	};

	/// @brief 同じ建設費で曲線列を評価する。勾配・曲率・水面クリアランス違反は無限大。
	double constructionCost(const World& world, const Array<CubicBezier>& curves, RoadType type, TransportMode mode = TransportMode::Road);
	/// @brief 端点を固定し、有限探索で見つかった最小費用の候補を返す。候補がなければ none。
	Optional<Result> find(const World& world, Vec3 start, Vec3 goal, RoadType type,
		int expansionLimit = 60000, TransportMode mode = TransportMode::Road, double maximumViaductHeight = Math::Inf);
	/// @brief 全曲線を標本化して路面と地盤の最大高低差を測る。
	double maximumClearance(const World& world, const Array<CubicBezier>& curves);
	/// @brief 地形の高低差を短絡している接続を再探索する。街の端点は動かさない。
	void repairSteepEdges(RoadNetwork& roads,const World& world);
	/// @brief 編集点から接線を共有する曲線列を作る。高さは弦への射影で勾配上限を保つ。
	Array<CubicBezier> fit(const Array<Vec3>& points);
	/// @brief 地形回廊を曲率まで連続する曲線へ変換し、実地盤で勾配と土工を検証する。
	Optional<Result> fitTerrain(const World& world, const Array<Vec3>& points, RoadType type, TransportMode mode = TransportMode::Road);
	/// @brief 曲線全体の勾配・曲率を検証する。
	bool respectsLimits(const CubicBezier& curve, RoadType type, TransportMode mode = TransportMode::Road);
}
