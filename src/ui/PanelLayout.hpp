#pragma once
#include <Siv3D.hpp>
#include <functional>

struct UIElement;
using UIElementPtr = std::unique_ptr<UIElement>;

// ===== ウィジェット構造体 =====

/// @brief 読み取り専用ラベル
struct PanelLabel
{
	String key;
	String text;                               ///< フォーマット文字列 "Speed: {:.1f} km/h"
	std::function<String()> source;            ///< 値を返すラムダ（参照バインド時は内部生成）
	bool bold = false;
	ColorF color{ 0.7 };
	String tooltip;
	int width = 0;                             ///< 0 = 自動
};

/// @brief クリックボタン
struct PanelButton
{
	String key;
	String label;
	std::function<bool()> active;              ///< アクティブ状態を返す
	String tooltip;
	int width = 0;
};

/// @brief ON/OFF トグル
struct PanelToggle
{
	String key;
	String labelOn;
	String labelOff;
	bool* ref = nullptr;                       ///< 参照バインド先
	String tooltip;
	int width = 0;
};

/// @brief 数値スピナー（ホイール増減）
struct PanelSpin
{
	String key;
	float* ref = nullptr;                      ///< 参照バインド先
	float step = 1.0f;
	float lo = 0.0f;
	float hi = 100.0f;
	String format = U"{:.1f}";
	String tooltip;
	int width = 0;
};

/// @brief 列挙サイクル（L/R クリック）
struct PanelCycle
{
	String key;
	int* ref = nullptr;                        ///< 参照バインド先（int にキャスト）
	Array<String> options;
	int count = 0;                             ///< options.size() のキャッシュ
	String tooltip;
	int width = 0;
};

/// @brief 固定スペース
struct PanelSpacer
{
	int height = 4;
};

/// @brief 区切り線
struct PanelSeparator {};

/// @brief 折りたたみセクション
struct PanelSection
{
	String key;
	String title;
	bool collapsed = false;
	ColorF color{ 1.0, 1.0, 0.4 };
	Array<UIElementPtr> children;
};

/// @brief カスタム描画ウィジェット
struct PanelCustom
{
	String key;
	int height = 0;                            ///< 0 = コールバックの戻り値で決定
	/// @param x, y, w パネルローカル座標と利用可能幅
	/// @return 実際に使用した高さ
	std::function<int(int x, int y, int w)> onDraw;
};

/// @brief 縦並びコンテナ
struct PanelVStack
{
	int gap = 0;
	int padding = 0;
	Array<UIElementPtr> children;
};

/// @brief 横並びコンテナ
struct PanelHStack
{
	int gap = 0;
	Array<UIElementPtr> children;
};

// ===== UIElement: 全ウィジェットの統一型 =====

using UIVariant = std::variant<
	PanelLabel, PanelButton, PanelToggle, PanelSpin, PanelCycle,
	PanelSpacer, PanelSeparator, PanelSection, PanelCustom,
	PanelVStack, PanelHStack>;

struct UIElement
{
	UIVariant widget;
	bool visible = true;

	// ── レイアウト計算結果（内部使用） ──
	int cx = 0, cy = 0, cw = 0, ch = 0;

	template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, UIElement>, int> = 0>
	UIElement(T&& w) : widget(std::forward<T>(w)) {}
};

// ===== PanelLayout: 宣言的パネル構築・レイアウト・描画・入力 =====

/// @brief パネル内 UI の宣言的構築と自動レイアウト
/// @details 構築後に update() → draw() を毎フレーム呼ぶ。
///   イベントは clicked() / changed() で取得する。
class PanelLayout
{
public:
	static constexpr int kDefaultHeight = 17;

	// ── 構築（ルート vstack に追加） ──

	/// @brief ラベル（ラムダソース）
	void label(StringView key, StringView text, std::function<String()> source,
	           ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ラベル（参照バインド: String）
	void label(StringView key, StringView text, String& ref,
	           ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ラベル（参照バインド: float）
	void label(StringView key, StringView text, float& ref,
	           ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ラベル（参照バインド: int）
	void label(StringView key, StringView text, int& ref,
	           ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ラベル（固定テキスト）
	void label(StringView key, StringView text,
	           ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ボタン（active ラムダ）
	void button(StringView key, StringView buttonLabel, std::function<bool()> active = nullptr,
	            int width = 0, StringView tooltip = U"");

	/// @brief ボタン（active 参照バインド）
	void button(StringView key, StringView buttonLabel, bool& activeRef,
	            int width = 0, StringView tooltip = U"");

	/// @brief トグル
	void toggle(StringView key, StringView labelOn, StringView labelOff, bool& ref,
	            int width = 0, StringView tooltip = U"");

	/// @brief スピン
	void spin(StringView key, float& ref, float step, float lo, float hi,
	          StringView format = U"{:.1f}", int width = 0);

	/// @brief サイクル
	template <typename E>
	void cycle(StringView key, E& ref, Array<String> options,
	           int width = 0, StringView tooltip = U"");

	/// @brief スペーサー
	void spacer(int height = 4);

	/// @brief 区切り線
	void separator();

	/// @brief カスタム描画
	void custom(StringView key, std::function<int(int, int, int)> onDraw, int height = 0);

	/// @brief VStack 開始
	void beginVStack(int gap = 0, int padding = 0);

	/// @brief HStack 開始
	void beginHStack(int gap = 0);

	/// @brief コンテナを閉じる
	void end();

	/// @brief セクション開始（折りたたみ可能）
	void beginSection(StringView key, StringView title, bool collapsed = false,
	                  ColorF color = ColorF{ 1.0, 1.0, 0.4 });

	/// @brief セクションを閉じる（end() と同義）
	void endSection();

	// ── 実行（毎フレーム） ──

	/// @brief 入力処理 + レイアウト再計算（dirty 時）
	/// @param availableWidth パネルのコンテンツ幅
	void update(int availableWidth);

	/// @brief 描画のみ
	void draw();

	// ── イベント取得 ──

	/// @brief ボタンがクリックされたか
	[[nodiscard]] bool clicked(StringView key) const;

	/// @brief 値が変化したか（spin, cycle, toggle）
	[[nodiscard]] bool changed(StringView key) const;

	/// @brief ウィジェットの表示/非表示を設定
	void setVisible(StringView key, bool vis);

	/// @brief コンテンツ全体の高さ（reportContentHeight 用）
	[[nodiscard]] int contentHeight() const { return m_contentHeight; }

private:
	// ルート要素群（暗黙の VStack）
	Array<UIElementPtr> m_root;

	// 構築スタック（beginVStack/HStack/Section 用）
	Array<Array<UIElementPtr>*> m_buildStack;

	// イベント記録
	HashSet<String> m_clickedKeys;
	HashSet<String> m_changedKeys;

	bool m_dirty = true;
	int  m_contentHeight = 0;
	int  m_availableWidth = 0;

	Array<UIElementPtr>& currentTarget();
	void addElement(UIVariant w);

	void doLayout(int availableWidth);
	void layoutElement(UIElement& elem, int x, int y, int w);
	int measureHeight(UIElement& elem, int w);

	// 描画
	void drawElement(const UIElement& elem);

	// 入力処理
	void updateElement(UIElement& elem);

	// キーで要素を検索
	UIElement* findByKey(StringView key);
};

// ── テンプレート実装 ──

template <typename E>
void PanelLayout::cycle(StringView key, E& ref, Array<String> opts,
                        int width, StringView tooltip)
{
	PanelCycle c;
	c.key = String{ key };
	c.ref = reinterpret_cast<int*>(&ref);
	c.count = static_cast<int>(opts.size());
	c.options = std::move(opts);
	c.tooltip = String{ tooltip };
	c.width = width;
	addElement(std::move(c));
}
