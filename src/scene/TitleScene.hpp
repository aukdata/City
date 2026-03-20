#pragma once
#include "SceneCommon.hpp"

/// @brief タイトルシーン（マップ生成パラメータ選択）
class TitleScene : public App::Scene
{
public:
	explicit TitleScene(const InitData& init);

	void update() override;
	void draw() const override;

private:
	// draw() const から SimpleGUI で変更されるため mutable
	mutable TextEditState m_seedTextState;
	mutable uint64        m_selectedSeed;
	mutable TerrainType   m_selectedTerrain;
	mutable bool          m_sandboxMode    = true;
	mutable bool          m_startRequested = false;
};
