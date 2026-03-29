#pragma once
#include <Siv3D.hpp>

/// @brief 物理状態: 路盤・構造物の建設状態（原則不可逆）
enum class BuildState : uint8
{
	NotBuilt,            ///< 路盤なし（計画のみ）
	UnderConstruction,   ///< 施工中（路盤未完成）
	Built,               ///< 路盤完成（供用可能）
	StubEnd,             ///< 延伸端・イカの耳
};
