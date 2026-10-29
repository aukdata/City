#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 自動車専用道路の横断面。寸法の正本は assets/generation/expressway.json。
/// @details 左側通行のため、一方通行の車道では運転者の左（負側）を路肩、右（正側）を中央分離帯とする。
///   取付道路は CSL の JP Bypass 24m（1+1 / 2+1）を移植した、中央分離帯付きの 24m 断面。
namespace ExpresswayCrossSection
{
	/// @brief 取付道路の車線構成。TwoOne は A→B 方向に 1 車線を追加する。
	enum class BypassVariant : uint8 { OneOne, TwoOne };

	namespace detail
	{
		inline RoadPart strip(RoadPartType type, float left, float width, StringView definition)
		{
			RoadPart part;
			part.type = type; part.defId = String{definition}; part.build = BuildState::Built;
			part.offsetA_L = part.offsetB_L = left;
			part.offsetA_R = part.offsetB_R = left + width;
			return part;
		}

		inline RoadPart repeat(float offset, StringView definition, float spacing)
		{
			RoadPart part;
			part.type = RoadPartType::RoadsideObject; part.defId = String{definition}; part.build = BuildState::Built;
			part.offsetA_L = part.offsetA_R = part.offsetB_L = part.offsetB_R = offset;
			part.placement = RoadPartPlacement::RepeatAlongEdge;
			part.envelopeRole = RoadPartEnvelopeRole::RoadOwnedObject;
			part.repeatSpacing = spacing; part.repeatJitter = 0;
			part.useDefinitionRepeatSpacing = false; part.useDefinitionRepeatJitter = false;
			return part;
		}

		inline Lane lane(float left, float width, LaneDir dir, LineType lineLeft, LineType lineRight)
		{
			Lane value;
			value.offsetA_L = value.offsetB_L = left;
			value.offsetA_R = value.offsetB_R = left + width;
			value.dir = dir; value.nominalWidth = width;
			value.lineLeft = lineLeft; value.lineRight = lineRight;
			return value;
		}

		/// @brief 同じ向きで隣り合う車線だけ車線変更を許可し、車両リストを車線数へ合わせる。
		inline void finish(RoadEdge& edge)
		{
			for (size_t i = 1; i < edge.lanes.size(); ++i)
			{
				const bool same = edge.lanes[i].dir == edge.lanes[i - 1].dir;
				edge.lanes[i].canChangeLaneLeft = same;
				edge.lanes[i - 1].canChangeLaneRight = same;
			}
			edge.laneVehicles = Array<Array<int>>(edge.lanes.size());
		}
	}

	/// @brief 本線の片側車道。車線中心を基準線とし、右端が中央分離帯の中心に一致する。
	inline float carriagewayOffset()
	{
		const auto& c = GenerationSettings::get();
		return c.expressway_lanesPerDirection * c.expressway_laneWidth * .5f + c.expressway_innerShoulder + c.expressway_medianWidth * .5f;
	}

	/// @brief 区間ごとに変わる路側設備。
	struct CarriagewayOptions
	{
		bool emergencyPhone = false; ///< 1km ごとの非常電話をこの区間の中央へ置く（トンネル内は 200m 間隔）
		bool noiseBarrier = false;   ///< 市街地に近い区間の遮音壁
	};

	/// @details 道路構造令 第1種第2級: 車線 3.5m、左側路肩 2.5m、中央帯 4.5m（側帯 0.75m×2＋分離帯 3.0m）。
	///   高架区間は法面・鋼製防護柵の代わりにコンクリート壁高欄を立てる。
	inline void applyCarriageway(RoadEdge& edge, const CarriagewayOptions& options)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const int count = c.expressway_lanesPerDirection;
		const float width = c.expressway_laneWidth, half = count * width * .5f;
		edge.roadType = RoadType::Expressway;
		edge.speedLimit = c.expressway_speed;
		edge.lanes.clear();
		for (int i = 0; i < count; ++i)
		{
			edge.lanes << lane(-half + i * width, width, LaneDir::Forward,
				i == 0 ? LineType::SolidWhite : LineType::DashedWhite, i + 1 == count ? LineType::SolidWhite : LineType::DashedWhite);
		}
		edge.parts.clear();
		const bool viaduct = edge.useElevation && !edge.tunnel;
		float x = -half - c.expressway_outerShoulder - c.expressway_guardrailWidth;
		if (!viaduct) { edge.parts << strip(RoadPartType::Slope, x - c.expressway_slopeWidth, c.expressway_slopeWidth, U"slope_grass"); }
		edge.parts << strip(viaduct ? RoadPartType::Wall : RoadPartType::Guardrail, x, c.expressway_guardrailWidth, viaduct ? U"wall_concrete" : U"guardrail_steel"); x += c.expressway_guardrailWidth;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_outerShoulder, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Roadbed, -half, half * 2, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Shoulder, half, c.expressway_innerShoulder, U"roadbed_asphalt");
		// 中央分離帯: 側帯の外に防護柵、分離帯の中心に防眩板（対向車の前照灯を遮る緑の羽板）。
		const float medianStart = half + c.expressway_innerShoulder, medianHalf = c.expressway_medianWidth * .5f;
		edge.parts << strip(RoadPartType::Guardrail, medianStart, c.expressway_guardrailWidth, U"guardrail_steel");
		edge.parts << strip(RoadPartType::Median, medianStart + c.expressway_guardrailWidth, medianHalf - c.expressway_guardrailWidth, U"median_concrete");
		edge.parts << repeat(medianStart + medianHalf - .05f, U"expressway_antiglare", c.expressway_antiGlareSpacing);
		// 路側設備（CSL road_kit / 日本道路テーマ）: 視線誘導標は左側が白、中央分離帯側が橙。
		const float rail = -half - c.expressway_outerShoulder - c.expressway_guardrailWidth * .5f;
		edge.parts << repeat(rail, U"expressway_delineator", c.expressway_delineatorSpacing);
		edge.parts << repeat(medianStart + c.expressway_guardrailWidth * .5f, U"expressway_delineator_median", c.expressway_delineatorSpacing);
		if (edge.tunnel || options.emergencyPhone)
		{
			edge.parts << repeat(rail - c.expressway_guardrailWidth - .55f, U"expressway_sos",
				edge.tunnel ? c.expressway_tunnelEmergencyPhoneSpacing : Max(4.0f, edge.length));
		}
		if (options.noiseBarrier && !edge.tunnel)
		{
			edge.parts << repeat(rail - c.expressway_guardrailWidth * .5f - .3f, U"expressway_noise_barrier", c.expressway_noiseBarrierPanel);
		}
		edge.parts << repeat(half + c.expressway_innerShoulder + .35f, U"expressway_kilopost", c.expressway_kilometrePostSpacing);
		finish(edge);
	}

	/// @brief IC・JCT の 1 車線ランプ。立体分岐（CSL grade_split）の車線幅と勾配を共有する。
	inline void applyRamp(RoadEdge& edge, float speed)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const float width = c.expressway_rampLaneWidth, half = width * .5f;
		edge.roadType = RoadType::Expressway;
		edge.speedLimit = speed;
		edge.lanes = { lane(-half, width, LaneDir::Forward, LineType::SolidWhite, LineType::SolidWhite) };
		edge.parts.clear();
		const float rail = c.expressway_guardrailWidth, slope = c.expressway_slopeWidth * .7f;
		float x = -half - c.expressway_rampOuterShoulder - rail - slope;
		edge.parts << strip(RoadPartType::Slope, x, slope, U"slope_grass"); x += slope;
		edge.parts << strip(RoadPartType::Guardrail, x, rail, U"guardrail_steel"); x += rail;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_rampOuterShoulder, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Roadbed, -half, width, U"roadbed_asphalt");
		x = half;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_rampInnerShoulder, U"roadbed_asphalt"); x += c.expressway_rampInnerShoulder;
		edge.parts << strip(RoadPartType::Guardrail, x, rail, U"guardrail_steel"); x += rail;
		edge.parts << strip(RoadPartType::Slope, x, slope, U"slope_grass");
		edge.parts << repeat(-half - c.expressway_rampOuterShoulder - rail * .5f, U"expressway_delineator", c.expressway_delineatorSpacing);
		// IC・JCT のランプは照明区間（本線は原則無照明）。
		edge.parts << repeat(-half - c.expressway_rampOuterShoulder - rail - .6f, U"expressway_light", c.expressway_rampLightSpacing);
		finish(edge);
	}

	/// @brief CSL JP Bypass 24m。車線 3.25m・歩道 3m・路肩 1.75m、中央分離帯 8m（2+1 は 4.75m）。
	inline void applyBypass(RoadEdge& edge, BypassVariant variant)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const float width = c.expressway_accessLaneWidth;
		const int forward = variant == BypassVariant::TwoOne ? 2 : 1;
		const float median = variant == BypassVariant::TwoOne ? c.expressway_accessAddedLaneMedian : c.expressway_accessMedian;
		const float outer = c.expressway_accessShoulder + c.expressway_accessSidewalk;
		// 片側だけ車線が増える版も全幅 24m の中心に揃え、1+1 との接続で歩道端を連続させる。
		const float left = -(forward * width + median * .5f + outer), right = width + median * .5f + outer;
		const float shift = -(left + right) * .5f;
		edge.roadType = RoadType::Arterial;
		edge.speedLimit = c.expressway_accessSpeed;
		edge.lanes.clear();
		for (int i = 0; i < forward; ++i)
		{
			const float start = shift - median * .5f - (forward - i) * width;
			edge.lanes << lane(start, width, LaneDir::Forward, i == 0 ? LineType::SolidWhite : LineType::DashedWhite,
				i + 1 == forward ? LineType::SolidWhite : LineType::DashedWhite);
		}
		edge.lanes << lane(shift + median * .5f, width, LaneDir::Backward, LineType::SolidWhite, LineType::SolidWhite);
		edge.parts.clear();
		constexpr float kCurb = .18f;
		const float walk = Max(.5f, c.expressway_accessSidewalk - kCurb);
		float x = shift + left;
		edge.parts << strip(RoadPartType::Sidewalk, x, walk, U"sidewalk_tile"); x += walk;
		edge.parts << strip(RoadPartType::Curb, x, kCurb, U"curb_concrete"); x += kCurb;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_accessShoulder, U"roadbed_asphalt"); x += c.expressway_accessShoulder;
		edge.parts << strip(RoadPartType::Roadbed, x, forward * width, U"roadbed_asphalt"); x += forward * width;
		edge.parts << strip(RoadPartType::Median, x, median, U"median_concrete"); x += median;
		edge.parts << strip(RoadPartType::Roadbed, x, width, U"roadbed_asphalt"); x += width;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_accessShoulder, U"roadbed_asphalt"); x += c.expressway_accessShoulder;
		edge.parts << strip(RoadPartType::Curb, x, kCurb, U"curb_concrete"); x += kCurb;
		edge.parts << strip(RoadPartType::Sidewalk, x, walk, U"sidewalk_tile");
		finish(edge);
	}
}
