#pragma once
#include <Siv3D.hpp>
#include "RoadTypes.hpp"

/// @brief 道路テンプレートのプリセット（敷設モードで保存・呼び出し可能な設定）
struct RoadTemplatePreset
{
	String          name;
	RoadType        roadType   = RoadType::LocalRoad;
	float           speedLimit = 60.0f;
	Array<RoadPart> parts;
	Array<Lane>     lanes;

	/// @brief RoadEdge の現在値からプリセットを生成する
	static RoadTemplatePreset fromEdge(const RoadEdge& edge, const String& name);

	/// @brief プリセットの内容を dst エッジに適用する（id/ノード/制御点/長さ/planId/cutoff は変更しない）
	void applyTo(RoadEdge& dst) const;

	/// @brief "{RoadType} {lanes}車線 {speed}km/h" 形式の自動命名文字列を返す
	static String autoName(const RoadEdge& edge);
};

/// @brief お気に入り・デフォルトプリセットの管理クラス
class RoadPresetStore
{
public:
	/// @brief "user_data/draw_presets.json" から読み込む（ファイルが存在しなければ何もしない）
	void load();

	/// @brief "user_data/draw_presets.json" に保存する（失敗時は Logger 警告）
	bool save() const;

	[[nodiscard]] const Array<RoadTemplatePreset>& favorites() const { return m_favorites; }
	[[nodiscard]] const Array<RoadTemplatePreset>& defaults()  const { return m_defaults; }

	/// @brief お気に入りに追加する（名前重複時は "(2)" "(3)" ... を付加）
	void addFavorite(RoadTemplatePreset preset);

	/// @brief 指定インデックスのお気に入りを削除する
	void removeFavoriteAt(size_t index);

	/// @brief ハードコードされたデフォルトプリセット4種を構築して返す
	static Array<RoadTemplatePreset> buildDefaults();

private:
	Array<RoadTemplatePreset> m_favorites;
	Array<RoadTemplatePreset> m_defaults = buildDefaults();
};
