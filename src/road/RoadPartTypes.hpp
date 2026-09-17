#pragma once
#include "RoadEnums.hpp"

// ===== 道路部品の型定義 =====
// 仕様: plan/16_road_cross_section_spec.md

/// @brief 道路部品の種別
enum class RoadPartType : uint8
{
	Roadbed = 0,         ///< 路盤（車線が乗る面）
	Shoulder = 1,        ///< 路肩
	Median = 2,          ///< 中央分離帯
	Sidewalk = 3,        ///< 歩道
	Gutter = 4,          ///< 側溝
	Guardrail = 5,       ///< ガードレール・防護柵
	Wall = 6,            ///< 擁壁・橋の欄干
	Curb = 7,            ///< 縁石
	Slope = 8,           ///< のり面（盛土・切土）
	BikeLane = 9,        ///< 自転車レーン
	UtilityPole = 10,    ///< 電柱・街灯柱などの沿道反復部品
	RoadsideGutter = 11, ///< 縁石横の立体側溝
	RoadsideObject = 12, ///< カーブミラー・バス停などの沿道付属物
};

/// @brief タイリングモード
enum class TilingMode : uint8
{
	CrossSection,   ///< 断面押し出し方式（路盤・歩道等）
	Longitudinal,   ///< 長手方向タイル方式（ガードレール等）
};

/// @brief 道路部品の配置方式
/// @details Strip は道路構造帯、RepeatAlongEdge は道路が所有する長手方向反復部品。
enum class RoadPartPlacement : uint8
{
	Strip = 0,
	RepeatAlongEdge = 1,
};

/// @brief 道路部品が道路構造幅へ寄与するか
/// @details RoadOwnedObject は terrain subtraction / node cap / parcel exclusion の幅へ含めない。
enum class RoadPartEnvelopeRole : uint8
{
	Structural = 0,
	RoadOwnedObject = 1,
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
	RoadPartPlacement placement = RoadPartPlacement::Strip; ///< 既定の配置方式
	RoadPartEnvelopeRole envelopeRole = RoadPartEnvelopeRole::Structural; ///< 既定の構造幅ロール
	float          modelUnitWidth = 0.5f;        ///< center メッシュ1個の幅 [m]
	float          modelUnitLen   = 1.0f;        ///< Longitudinal 用: 1タイルの長さ [m]
	float          repeatSpacing  = 20.0f;       ///< RepeatAlongEdge 用: 反復間隔 [m]
	float          repeatJitter   = 0.0f;        ///< RepeatAlongEdge 用: 決定論的な間隔ゆらぎ [m]
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
	RoadPartPlacement placement = RoadPartPlacement::Strip; ///< 配置方式
	RoadPartEnvelopeRole envelopeRole = RoadPartEnvelopeRole::Structural; ///< 構造幅への寄与
	float repeatSpacing = 20.0f;                         ///< 反復配置時の間隔 [m]
	float repeatJitter = 0.0f;                           ///< 反復配置時の決定論的ゆらぎ [m]
	bool useDefinitionRepeatSpacing = true;              ///< true: RoadPartDef の repeatSpacing を使う
	bool useDefinitionRepeatJitter = true;               ///< true: RoadPartDef の repeatJitter を使う

	/// @brief 舗装用シェーダを使う路盤。バラスト・スラブは独自の材質で描く。
	[[nodiscard]] bool asphalt() const { return type == RoadPartType::Roadbed && (defId.isEmpty() || defId == U"roadbed_asphalt"); }

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
