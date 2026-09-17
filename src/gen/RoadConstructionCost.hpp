#pragma once
#include "GenerationSettings.hpp"
#include <Siv3D.hpp>
#include <cmath>

/// @brief 道路の単位延長費。金額ではなく地上を1としたユーザー指定の比較係数。
namespace RoadConstructionCost
{
	inline double Surface() { return GenerationSettings::get().roads_surfaceCost; }
	inline double Earthwork() { return GenerationSettings::get().roads_earthworkCost; }
	inline double ViaductPerMetre() { return GenerationSettings::get().roads_viaductCostPerMetre; }
	inline double Tunnel() { return GenerationSettings::get().roads_tunnelCost; }
	inline double SurfaceTolerance() { return GenerationSettings::get().roads_surfaceTolerance; }
	inline double MaximumFill() { return GenerationSettings::get().roads_maximumFill; }
	inline double MaximumCut() { return GenerationSettings::get().roads_maximumCut; }

	/// @brief 短い構造物への影響を小さくし、長大構造物で急増する追加費用。
	inline double exponentialPenalty(double value, double scale, double weight)
	{
		if (weight <= 0 || value <= 0) { return 0; }
		const double ratio = value / scale;
		if (ratio > 200) { return Math::Inf; }
		return weight * (std::expm1(ratio) - ratio);
	}
	/// @brief 連続するトンネル全長に対する追加費用。エッジ分割ではリセットしない。
	inline double tunnelPenalty(double length)
	{
		const auto& config = GenerationSettings::get();
		return config.roads_tunnelLengthScale * exponentialPenalty(length,
			config.roads_tunnelLengthScale, config.roads_tunnelLengthPenalty);
	}

	/// @brief 高架は高さ比例費に指数罰則を加える。川底下の土被りがある区間はトンネル。
	inline double unit(double roadHeight, double ground, double water = -1e9)
	{
		const double difference = roadHeight - ground;
		if (difference < -MaximumCut()) { return Tunnel(); }
		if (difference > MaximumFill() || ground < water + GenerationSettings::get().crossings_waterBankMargin) { return GenerationSettings::get().roads_viaductBaseCost + Max(0.0, difference) * ViaductPerMetre() + exponentialPenalty(Max(0.0, difference), GenerationSettings::get().roads_viaductHeightScale, GenerationSettings::get().roads_viaductHeightPenalty); }
		if (Abs(difference) > SurfaceTolerance()) { return Earthwork(); }
		return Surface();
	}

	/// @brief 区間を積算し、現在の連続トンネル長を次区間へ渡す。
	inline double segment(double roadHeight, double ground, double water, double length, double& tunnelRun)
	{
		double cost = unit(roadHeight, ground, water) * length;
		if (ground - roadHeight > MaximumCut())
		{
			cost += tunnelPenalty(tunnelRun + length) - tunnelPenalty(tunnelRun);
			tunnelRun += length;
		}
		else { tunnelRun = 0; }
		return cost;
	}
}
