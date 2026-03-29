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
	float      width   = 0.0f;                  ///< この部品の幅 [m]
	float      offset  = 0.0f;                  ///< 道路中心からの左端位置 [m]（左がマイナス）
	BuildState build   = BuildState::Built;      ///< 建設状態

	/// @brief 部品種別を取得するヘルパー（RoadPartRegistry 不要の簡易版）
	/// @note defId からの逆引きが必要な場合は RoadPartRegistry を使用
	RoadPartType type = RoadPartType::Roadbed;
};
