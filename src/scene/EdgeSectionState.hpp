#pragma once

/// @brief 道路エッジ編集パネル（drawEdgePanel）の選択状態を他モジュール（3D ハンドル描画/入力）へ公開
namespace EdgeSectionState
{
	inline int selectedPart = -1;
	inline int selectedLane = -1;
}
