#pragma once
#include "MapGenerator.hpp"

/// @brief 農業立地と地形上の往来費用から、中心町・周辺集落の関係を生成する。
namespace SettlementPlacement
{
	struct Candidate
	{
		Vec2 center;
		float score = 0;
	};
	using HeightSampler = std::function<double(const Vec2&)>;

	/// @brief 地形候補を中心都市、地方町、農村へ配置する。同じ地形・seedでは同じ結果。
	Array<MapGenerator::Settlement> generate(uint64 seed, Array<Candidate> candidates,
		const RectF& bounds, const HeightSampler& height,
		const HeightSampler& water = [](const Vec2&) { return 0.0; });

	/// @brief 町村候補を位置に固有の乱数で間引く。市の採用には適用しない。
	bool retainRuralSite(uint64 seed,Vec2 center);

	/// @brief 道路への接続候補を、距離と中心町への地形回廊の方向で評価する。
	double roadAccessCost(Vec2 from, Vec2 target, const Optional<Vec2>& preferredDirection);
}
