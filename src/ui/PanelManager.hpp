#pragma once
#include <Siv3D.hpp>

/// @brief パネルの状態
struct PanelState
{
	String id;
	String title;
	Vec2   pos;
	Vec2   size;
	bool   visible    = false;
	bool   movable    = false;
	bool   scrollable = false;
	int    zOrder     = 0;
	double scrollOffset  = 0.0;
	double contentHeight = 0.0;
};

/// @brief beginContent() の RAII ガード（スコープ離脱でシザー/変換を解除）
struct ScopedContentArea
{
	ScopedRenderStates2D scissor;   ///< シザーレクトによるクリッピング
	Transformer2D        transform; ///< 座標変換（描画+カーソル）
};

/// @brief 複数パネルの統一管理（Z オーダー・入力遮蔽・スクロール・ドラッグ）
class PanelManager
{
public:
	static constexpr int kTitleBarH = 24;

	/// @brief パネルを登録する（初回のみ）
	void registerPanel(StringView id, Vec2 size, bool movable, bool scrollable = false);

	/// @brief パネルを表示する（最前面に配置）
	void show(StringView id, StringView title, Vec2 pos);

	/// @brief パネルを閉じる
	void hide(StringView id);

	/// @brief 表示中かどうか
	[[nodiscard]] bool isVisible(StringView id) const;

	/// @brief 入力処理（閉じる・ドラッグ・Z オーダー・スクロール）
	/// @return いずれかのパネルが入力を消費したら true
	bool handleInput();

	/// @brief 全パネルの背景・タイトルバーを描画（zOrder 昇順）
	void drawBackgrounds();

	/// @brief スクロール対応のコンテンツ描画を開始する
	/// @return RAII ガード。visible でなければ none
	[[nodiscard]] Optional<ScopedContentArea> beginContent(StringView id);

	/// @brief コンテンツの実際の高さを報告する
	void reportContentHeight(StringView id, double height);

	/// @brief マウスがいずれかの表示中パネル上にあるか
	[[nodiscard]] bool isMouseOnAnyPanel() const;

private:
	HashTable<String, PanelState> m_panels;
	int    m_nextZOrder = 0;
	String m_draggingId;
	Vec2   m_dragOffset;

	Font m_titleFont{ FontMethod::MSDF, 14, Typeface::Bold };

	PanelState* find(StringView id);
	const PanelState* find(StringView id) const;

	/// @brief 表示中パネルを zOrder 昇順で返す（描画用）
	Array<PanelState*> sortedPanels();
};
