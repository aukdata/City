#pragma once
#include "LinearNetworkStyle.hpp"

/// @brief 道路の外観・寸法スタイル定義
/// 実際の車線数は RoadEdge.lanes が保持するため、ここでは定義しない。
/// TOML からロードされ、RoadStyleRegistry に格納される。
struct RoadStyle
{
	LinearNetworkStyle surface;                         ///< 路面テクスチャ・色・UV タイリング

	// ---- 断面寸法 ----
	float defaultLaneWidth = 3.5f;   ///< 標準車線幅 [m]（新規道路追加時のデフォルト）

	// ---- 車線区画線スタイル ----
	LineMarkStyle laneMarking;       ///< 同方向車線間の区画線（白破線など）
	LineMarkStyle centerLine;        ///< 対向車線境界のセンターライン（黄実線など）
};
