#pragma once

/// @brief 道路エッジ編集パネル（drawEdgePanel）の選択状態を他モジュール（3D ハンドル描画/入力）へ公開
namespace EdgeSectionState
{
	// UI と 3D 操作の橋渡し用に、現在選択されている部品と車線だけを共有する。
	inline int selectedPart = -1;
	inline int selectedLane = -1;
}
