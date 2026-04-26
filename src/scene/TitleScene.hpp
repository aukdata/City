#pragma once
#include "SceneCommon.hpp"

/// @brief タイトルシーン（マップ生成パラメータ選択 + セーブデータロード）
class TitleScene : public App::Scene
{
public:
	// タイトル画面では開始条件の一時 UI 状態を保持し、確定時だけ SceneData へ反映する。
	explicit TitleScene(const InitData& init);

	void update() override;
	void draw() const override;

private:
	mutable TextEditState m_seedTextState;
	mutable uint64        m_selectedSeed;
	mutable bool          m_sandboxMode    = true;
	mutable bool          m_startRequested = false;

	// セーブデータ
	mutable Array<String> m_saveNames;
	mutable int           m_selectedSave   = -1;
	mutable bool          m_loadRequested  = false;

	void scanSaves() const;
};
