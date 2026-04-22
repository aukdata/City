#pragma once
#include "RoadEnums.hpp"

// ===== 道路部品の型定義 =====
// 仕様: plan/16_road_cross_section_spec.md

/// @brief 道路部品の種別
enum class RoadPartType : uint8
{
	Roadbed,        ///< 路盤（車線が乗る面）
	Shoulder,       ///< 路肩
	Median,         ///< 中央分離帯
	Sidewalk,       ///< 歩道
	Gutter,         ///< 側溝
	Guardrail,      ///< ガードレール・防護柵
	Wall,           ///< 擁壁・橋の欄干
	Curb,           ///< 縁石
	Slope,          ///< のり面（盛土・切土）
	BikeLane,       ///< 自転車レーン
};

/// @brief タイリングモード
enum class TilingMode : uint8
{
	CrossSection,   ///< 断面押し出し方式（路盤・歩道等）
	Longitudinal,   ///< 長手方向タイル方式（ガードレール等）
};

/// @brief OBJ パース後のメッシュデータ（1オブジェクト分）
struct PartModelData
{
	String                   name;       ///< OBJ 内のオブジェクト名
	Array<Vertex3D>          vertices;
	Array<TriangleIndex32>   indices;

	[[nodiscard]] bool isEmpty() const { return vertices.isEmpty(); }
};

/// @brief 道路部品の3Dモデル（inner/center/outer + LOD）
struct RoadPartModel
{
	PartModelData inner;    ///< 道路中心に近い側の端メッシュ
	PartModelData center;   ///< 幅方向に繰り返すメッシュ
	PartModelData outer;    ///< 道路中心から遠い側の端メッシュ（symmetric 時は空）

	/// @brief LOD 段階（lods[0]=LOD1, lods[1]=LOD2, ...）
	Array<RoadPartModel> lods;
};

/// @brief 道路部品アセット定義（TOML + OBJ のペア）
struct RoadPartDef
{
	String         id;                           ///< 一意な識別子
	String         name;                         ///< 表示名
	RoadPartType   type   = RoadPartType::Roadbed;
	TilingMode     tiling = TilingMode::CrossSection;
	float          modelUnitWidth = 0.5f;        ///< center メッシュ1個の幅 [m]
	float          modelUnitLen   = 1.0f;        ///< Longitudinal 用: 1タイルの長さ [m]
	float          heightOffset   = 0.0f;        ///< 路面基準からの高低差 [m]
	bool           symmetric      = false;       ///< true: outer を inner のミラーで生成

	String         modelPath;                    ///< OBJ ファイルパス
	String         texturePath;                  ///< テクスチャパス
	ColorF         color = ColorF{ 0.5 };        ///< 基本色

	Array<String>  lodMeshNames;                 ///< LOD 段階のメッシュ名
	Array<float>   lodDistances;                 ///< LOD 切替距離 [m]

	Optional<Texture> texture;                   ///< ロード後に格納
};

/// @brief 道路上の部品インスタンス
struct RoadPart
{
	String     defId;                            ///< RoadPartDef への参照
	/// @brief A 端（開始側）の左端オフセット [m]（道路中心から左がマイナス）
	float      offsetA_L = 0.0f;
	/// @brief A 端（開始側）の右端オフセット [m]
	float      offsetA_R = 0.0f;
	/// @brief B 端（終了側）の左端オフセット [m]
	float      offsetB_L = 0.0f;
	/// @brief B 端（終了側）の右端オフセット [m]
	float      offsetB_R = 0.0f;
	BuildState build   = BuildState::Built;      ///< 建設状態
	RoadPartType type = RoadPartType::Roadbed;   ///< 部品種別

	/// @brief A 端での幅
	[[nodiscard]] float widthA()   const { return offsetA_R - offsetA_L; }
	/// @brief B 端での幅
	[[nodiscard]] float widthB()   const { return offsetB_R - offsetB_L; }
	/// @brief 代表幅（一様幅の場合に使用）
	[[nodiscard]] float width()    const { return (widthA() + widthB()) * 0.5f; }
	/// @brief 代表左端オフセット（平均）
	[[nodiscard]] float offsetL()  const { return (offsetA_L + offsetB_L) * 0.5f; }
	/// @brief 代表右端オフセット（平均）
	[[nodiscard]] float offsetR()  const { return (offsetA_R + offsetB_R) * 0.5f; }
};

// ===== 道路オブジェクト =====

/// @brief 道路オブジェクト種別
enum class RoadObjectType : uint8
{
	Pier,           ///< 橋脚
	// 将来拡張: StreetLight, TrafficSign, UtilityPole, StreetTree, ...
};

/// @brief 道路に紐づく汎用オブジェクト
struct RoadObject
{
	int             id = -1;
	int             parentEdgeId = -1;      ///< 親エッジ ID
	float           arcPos = 0.0f;          ///< エッジ上の弧長位置 [m]
	float           lateralOffset = 0.0f;   ///< 横方向オフセット [m]
	RoadObjectType  type = RoadObjectType::Pier;
	float           scale = 1.0f;
	float           yawOffset = 0.0f;       ///< Y軸回転オフセット [rad]
	float           heightOverride = -1.0f; ///< -1 = 地形まで自動延伸
};
