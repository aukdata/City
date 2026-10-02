#pragma once
#include <Siv3D.hpp>
#include "../asset/AssetRegistrar.hpp"

/// @brief パネルの状態
struct PanelState
{
	// パネルの表示状態と入力補助情報を 1 レコードにまとめ、ID で直接引けるようにする。
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
	// 各 UI パネルの表示順・入力遮蔽・スクロールを一元管理し、個別パネル実装を薄く保つ。
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
	bool handleInput(Vec2 cursor = Cursor::PosF(), bool clicked = MouseL.down(),
		bool held = MouseL.pressed(), double wheel = Mouse::Wheel());

	/// @brief 全パネルの背景・タイトルバーを描画（zOrder 昇順）
	void drawBackgrounds();

	/// @brief 指定パネルの背景・タイトルバーを描画する
	void drawBackground(StringView id);

	/// @brief 表示中パネルの ID を zOrder 昇順で返す
	[[nodiscard]] Array<String> sortedPanelIds() const;

	/// @brief スクロール対応のコンテンツ描画を開始する
	/// @return RAII ガード。visible でなければ none
	[[nodiscard]] Optional<ScopedContentArea> beginContent(StringView id);

	/// @brief コンテンツの実際の高さを報告する
	void reportContentHeight(StringView id, double height);

	/// @brief マウスがいずれかの表示中パネル上にあるか
	[[nodiscard]] bool isMouseOnAnyPanel(Vec2 cursor = Cursor::PosF()) const;

	/// @brief 直前の handleInput() でクリックが消費されたか
	[[nodiscard]] bool consumedInput() const { return m_consumedInput; }

	/// @brief マウス操作がパネルにより吸収されるか（パネル上にカーソルがある or 入力消費済み）
	/// @details 3D空間のクリック・ドラッグ処理は、このフラグが true のときスキップすべき
	[[nodiscard]] bool blocksMouseInput() const { return isMouseOnAnyPanel() || m_consumedInput; }

	/// @brief 登録済みパネルのサイズを返す（未登録なら {0,0}）
	[[nodiscard]] Vec2 getSize(StringView id) const
	{
		const auto* p = find(id);
		return p ? p->size : Vec2{0, 0};
	}

private:
	bool   m_consumedInput = false;
	HashTable<String, PanelState> m_panels;
	int    m_nextZOrder = 0;
	String m_draggingId;
	Vec2   m_dragOffset;
	String m_mouseOwner;  ///< カーソル位置で最前面のパネル ID

	Font m_titleFont = FontAsset(Asset::PanelBold14);

	PanelState* find(StringView id);
	const PanelState* find(StringView id) const;

	/// @brief 表示中パネルを zOrder 昇順で返す（描画用）
	Array<PanelState*> sortedPanels();
};
