#pragma once
#include <Siv3D.hpp>
#include "../road/RoadTypes.hpp"

class RoadNetwork;
class RoadRenderer;
class PanelManager;
class PanelBuilder;

/// @brief 案内標識の編集状態とパネル描画を集約するクラス
/// @details GameScene から看板関連の状態とパネル描画ロジックを分離する。
///
/// 責務:
/// - 編集中の標識 ID + WYSIWYG 選択・ドラッグ状態の保持
/// - エッジパネルの案内標識セクション描画
/// - 看板プロパティパネル（guide_sign_edit）描画
/// - WYSIWYG エディタパネル（guide_sign_editor）描画
class GuideSignEditor
{
public:
	/// @brief 編集パネルが開いているか
	[[nodiscard]] bool isOpen() const { return m_editingSignId >= 0; }

	/// @brief 編集中の看板 ID（-1 = 未編集）
	[[nodiscard]] int editingId() const { return m_editingSignId; }

	/// @brief 編集パネルを開く（ヒットテスト成功時に呼ぶ）
	/// @details TextEditState を看板のエントリ数に合わせて初期化する
	void open(int signId, const RoadNetwork& network);

	/// @brief 編集パネルを閉じる（選択解除時）
	void close();

	/// @brief エッジパネル内の案内標識セクションを描画
	/// @return データに変更があった場合 true（呼び出し側で invalidateEdgeCache）
	bool drawEdgeSection(PanelBuilder& ui, RoadEdge& edge,
	                     RoadNetwork& network, PanelManager& panelManager,
	                     const RoadRenderer& renderer);

	/// @brief 看板プロパティパネル（guide_sign_edit）の中身を描画
	/// @details panelManager の beginContent/reportContentHeight を内部で呼ぶ
	/// @return 変更があった場合 true
	bool drawEditPanel(RoadNetwork& network, PanelManager& panelManager,
	                   const RoadRenderer& renderer);

	/// @brief WYSIWYG エディタパネル（guide_sign_editor）の中身を描画
	/// @return 変更があった場合 true
	bool drawEditorPanel(RoadNetwork& network, PanelManager& panelManager,
	                     const RoadRenderer& renderer);

private:
	// ── 編集状態 ──
	int  m_editingSignId    = -1;   ///< 編集中の GuideSign ID
	int  m_selectedElement  = -1;   ///< WYSIWYG: elements 内の選択要素 index
	bool m_dragging         = false;
	bool m_copyDrag         = false;
	Vec2 m_dragOffset       { 0, 0 };  ///< ドラッグ開始時のマウス→要素オフセット
	Vec2 m_dragStartNorm    { 0, 0 };  ///< ドラッグ開始時の正規化位置（Shift 軸固定用）

	// ── 看板エディタの deferred-save 用 draft ──
	Array<SignElement> m_draftElements;
	bool               m_draftValid = false;

	// ── WYSIWYG プレビュー用 draft テクスチャ ──
	// m_draftElements が変わるたびに m_draftTexDirty = true にセットし、
	// 次の drawEditorPanel 呼び出し時に再生成する。
	RenderTexture m_draftTex;
	bool          m_draftTexDirty = true;

	// ── 選択中要素のテキスト入力状態 ──
	int           m_lastSelectedElement = -1;
	TextEditState m_textEditState;
	TextEditState m_readingEditState;

	// ── 内部ヘルパー ──
	bool isDraftSelValid() const
	{
		return m_selectedElement >= 0
			&& m_selectedElement < static_cast<int>(m_draftElements.size());
	}

	/// @brief 必要なら m_draftTex を m_draftElements から再生成する
	/// @details パネル座標変換の影響を受けないよう beginContent の前に呼ぶこと
	void regenerateDraftTextureIfDirty(const GuideSignPlacement& g);

	/// @brief 指定種別の新規要素をツールバーから追加する
	void addElementFromToolbar(SignElementKind kind);

	/// @brief 選択中要素を削除し、選択状態をクリアする
	void deleteSelectedElement();
};
