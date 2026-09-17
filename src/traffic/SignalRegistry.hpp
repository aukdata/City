#pragma once
#include <Siv3D.hpp>
#include "../road/ObjParser.hpp"

/// @brief 信号ランプの状態定義
struct SignalState
{
	String id;       ///< 状態名 ("red", "green", "arrow_left" 等)
	Float4 uvRect;   ///< テクスチャ内のピクセル矩形 {x, y, w, h}
};

/// @brief 1つのランプスロット定義
struct SignalLampDef
{
	String        meshName;    ///< OBJ 内のオブジェクト名
	Array<String> stateIds;    ///< このランプが取りうる状態 ID リスト
};

/// @brief sub_lamp（矢印信号等）の定義
struct SignalSubLampDef
{
	String        meshName;      ///< ランプ面メッシュ名
	String        bodyMeshName;  ///< 矢印灯器筐体メッシュ名
	Array<String> stateIds;      ///< 取りうる状態 ID リスト
	Float3        colStride;     ///< 同一行内でのオフセット
	Float3        rowStride;     ///< 次の行へのオフセット
	int           cols = 3;      ///< 1行あたり最大数
};

/// @brief 信号機アセット定義（TOML から読み込み）
struct SignalDef
{
	String id;
	String name;
	String modelPath;
	String texturePath;
	String bodyMeshName;       ///< 筐体メッシュ名

	Array<SignalLampDef>          lamps;     ///< メインランプ定義
	Optional<SignalSubLampDef>    subLamp;   ///< sub_lamp 定義
	HashTable<String, SignalState> states;   ///< 状態名 → 状態定義
};

/// @brief 信号機の読み込み済みモデルデータ
struct SignalModel
{
	HashTable<String, PartModelData> meshes;  ///< メッシュ名 → メッシュデータ
	Optional<Texture>                texture;
	std::array<HashTable<String, PartModelData>, 2> lodMeshes;
};

/// @brief 信号アセット管理レジストリ
class SignalRegistry
{
public:
	/// @brief App/assets/signals/ 内の TOML+OBJ を読み込む
	bool load(FilePathView dirPath);

	/// @brief 定義を取得
	[[nodiscard]] const SignalDef* getDef(StringView id) const;

	/// @brief モデルを取得
	[[nodiscard]] const SignalModel* getModel(StringView id) const;

	/// @brief 登録済み ID 一覧
	[[nodiscard]] Array<String> defIds() const;

	/// @brief 登録数
	[[nodiscard]] size_t size() const { return m_entries.size(); }

private:
	struct Entry
	{
		SignalDef   def;
		SignalModel model;
	};

	HashTable<String, Entry> m_entries;

	Optional<Entry> loadEntry(FilePathView tomlPath, FilePathView baseDir);
};
