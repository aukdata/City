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
		bool cutting = false;        ///< 切土区間。法面は地形の掘削で表れるため盛土用の法面を付けない
		bool lighting = false;       ///< IC・JCT の分合流部の前後。本線にも道路照明を立てる
		/// @brief 変速車線（加速・減速車線）の幅 [m]。A 端と B 端で変え、すり付け区間は幅を 0 へ絞る。
		/// 本線の左（路肩側）に 1 車線を足し、路肩・防護柵・法面をその分外へ出す。
		float auxiliaryA = 0, auxiliaryB = 0;
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
		if (!viaduct && !options.cutting) { edge.parts << strip(RoadPartType::Slope, x - c.expressway_slopeWidth, c.expressway_slopeWidth, U"slope_grass"); }
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
		if (options.lighting && !edge.tunnel)
		{
			// 照明柱は路肩側の防護柵の外。高架では壁高欄の上に立てる。
			edge.parts << repeat(rail - c.expressway_guardrailWidth * .5f - (viaduct ? 0.0f : .6f), U"expressway_light", c.expressway_mainlineLightSpacing);
		}
		if (options.auxiliaryA > 0 || options.auxiliaryB > 0)
		{
			// 変速車線: 本線の左端より外の部材を変速車線の幅だけ外へずらし、車道を左へ広げる。
			const float a = options.auxiliaryA, b = options.auxiliaryB;
			for (auto& part : edge.parts)
			{
				if (part.type == RoadPartType::Roadbed) { part.offsetA_L -= a; part.offsetB_L -= b; continue; }
				if (part.offsetA_R > -half + .01f) { continue; }
				part.offsetA_L -= a; part.offsetA_R -= a; part.offsetB_L -= b; part.offsetB_R -= b;
			}
			// 本線車線との境は太く周期の短い破線（変速車線の車線境界線）。
			Lane added = lane(-half - Max(a, b), Max(a, b), LaneDir::Forward, LineType::SolidWhite, LineType::DashedWhiteThick);
			added.offsetA_L = -half - a; added.offsetA_R = -half;
			added.offsetB_L = -half - b; added.offsetB_R = -half;
			edge.lanes.front().lineLeft = LineType::DashedWhiteThick;
			edge.lanes.insert(edge.lanes.begin(), added);
		}
		finish(edge);
	}

	/// @brief 道路の一端の車線と部材を横へずらす。lateral は寄せる量（負 = 左）。
	/// @param collapse +1: 右側（車線より外）の路肩・防護柵・法面をその端で幅 0 へ絞る。-1: 左側を絞る。0: 絞らない。
	/// @details 別の道路と路面を共有する端（変速車線へ入るランプ、2 本のランプが 1 本の道路へまとまる分合流の鼻）で、
	///   内側の路側部材が相手の車道へはみ出さないようにする。
	inline void shiftEnd(RoadEdge& edge, bool atB, float lateral, int collapse)
	{
		if (edge.lanes.isEmpty()) { return; }
		float laneMin = Math::Inf, laneMax = -Math::Inf;
		for (auto& lane : edge.lanes)
		{
			float& laneLeft = atB ? lane.offsetB_L : lane.offsetA_L;
			float& laneRight = atB ? lane.offsetB_R : lane.offsetA_R;
			laneMin = Min(laneMin, laneLeft); laneMax = Max(laneMax, laneRight);
			laneLeft += lateral; laneRight += lateral;
		}
		const float center = (laneMin + laneMax) * .5f;
		for (auto& part : edge.parts)
		{
			float& left = atB ? part.offsetB_L : part.offsetA_L;
			float& rightEdge = atB ? part.offsetB_R : part.offsetA_R;
			if (part.type == RoadPartType::Roadbed) { left += lateral; rightEdge += lateral; continue; }
			const bool rightSide = (left + rightEdge) * .5f >= center;
			if (collapse > 0 && rightSide) { left = rightEdge = laneMax + lateral; }
			else if (collapse < 0 && !rightSide) { left = rightEdge = laneMin + lateral; }
			else { left += lateral; rightEdge += lateral; }
		}
	}

	/// @brief ランプの一端を本線の変速車線へ寄せる。lateral は寄せる量（負 = 左）。
	/// @details 車線と左側の部材をずらし、右側（本線側）の路肩・防護柵・法面はその端で幅 0 へ絞る。
	inline void shiftRampEnd(RoadEdge& edge, bool atB, float lateral) { shiftEnd(edge, atB, lateral, 1); }

	/// @brief IC・JCT のランプ（既定 1 車線）。立体分岐（CSL grade_split）の車線幅と勾配を共有する。
	/// @param lanes 料金所と分合流の鼻を結ぶ区間は、料金所前の往復路の片側と同じ 2 車線。
	///   この区間は料金所広場の続きなので車線を区切る線を引かない。
	inline void applyRamp(RoadEdge& edge, float speed, int lanes = 1)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const int count = Max(1, lanes);
		const float width = c.expressway_rampLaneWidth, half = width * count * .5f;
		edge.roadType = RoadType::Expressway;
		edge.speedLimit = speed;
		edge.lanes.clear();
		for (int i = 0; i < count; ++i)
		{
			const bool plaza = count > 1;
			edge.lanes << lane(-half + i * width, width, LaneDir::Forward, plaza ? LineType::None : i == 0 ? LineType::SolidWhite : LineType::DashedWhite,
				plaza ? LineType::None : i + 1 == count ? LineType::SolidWhite : LineType::DashedWhite);
		}
		edge.parts.clear();
		const float rail = c.expressway_guardrailWidth, slope = c.expressway_slopeWidth * .7f;
		float x = -half - c.expressway_rampOuterShoulder - rail - slope;
		edge.parts << strip(RoadPartType::Slope, x, slope, U"slope_grass"); x += slope;
		edge.parts << strip(RoadPartType::Guardrail, x, rail, U"guardrail_steel"); x += rail;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_rampOuterShoulder, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Roadbed, -half, half * 2, U"roadbed_asphalt");
		x = half;
		edge.parts << strip(RoadPartType::Shoulder, x, c.expressway_rampInnerShoulder, U"roadbed_asphalt"); x += c.expressway_rampInnerShoulder;
		edge.parts << strip(RoadPartType::Guardrail, x, rail, U"guardrail_steel"); x += rail;
		edge.parts << strip(RoadPartType::Slope, x, slope, U"slope_grass");
		edge.parts << repeat(-half - c.expressway_rampOuterShoulder - rail * .5f, U"expressway_delineator", c.expressway_delineatorSpacing);
		// IC・JCT のランプは照明区間（本線は原則無照明）。
		edge.parts << repeat(-half - c.expressway_rampOuterShoulder - rail - .6f, U"expressway_light", c.expressway_rampLightSpacing);
		finish(edge);
	}

	/// @brief 料金所の連絡路（往復 2+2 車線・中央に分離帯）の全幅。
	inline float tollStemWidth()
	{
		const auto& c = GenerationSettings::get();
		return 4 * c.expressway_rampLaneWidth + 2 * c.expressway_rampOuterShoulder + 1.0f + 2 * c.expressway_guardrailWidth;
	}

	/// @brief 料金所前の往復路の中央分離帯の幅 [m]。
	inline constexpr float kTollSeparator = 1.0f;

	/// @brief 料金所レーンの種別。料金所の屋根の車線表示板（ETC 専用は紫、一般は緑、ETC/一般は両方）に対応する。
	enum class TollLaneKind : uint8 { EtcOnly, EtcGeneral, General };

	/// @brief 料金所の 1 レーン。横位置は連絡路の A→B 向きに対する左右（負 = 左）。
	struct TollLane
	{
		float left = 0, width = 0;
		TollLaneKind kind = TollLaneKind::EtcOnly;
		bool exit = true;     ///< 本線から一般道へ出る向き（A→B）。入口レーンは B→A
		int traffic = -1;     ///< 料金所区間の通行車線の番号。車両が通らないレーン（一般）は -1
	};

	/// @brief 料金所アイランド（ブースと柱を載せる縁石で囲った島）。along は屋根の中心からの A→B 方向の距離 [m]。
	struct TollIsland
	{
		float left = 0, width = 0, from = 0, to = 0;
	};

	/// @brief IC の料金所広場の平面形。寸法は NEXCO 設計要領 第六集 建築施設編（料金所）に合わせる。
	/// @details レーン幅は ETC レーンと最も左のレーンが 3.5m（標準 3.0m）。アイランドは幅 2.2m・縁石高 0.15m、
	///   長さは入口 ETC 48.5m・出口 32.5m。屋根は進行方向 11.9m、長辺の軒は最も外のアイランドの中心から 7.1m。
	///   ブースは 1 レーンに 1 棟、運転者の右のアイランドに置く（2.8 × 1.6 × 2.5m）。料金所事務所は出口側。
	///   IC の料金所は出口 3（一般・ETC/一般・ETC 専用）、入口 2（ETC/一般・ETC 専用）レーン。
	///   往復の境は中央のアイランドで、連絡路の分離帯の延長上に置く。
	struct TollPlazaLayout
	{
		static constexpr float kLaneWidth = 3.5f, kIslandWidth = 2.2f, kIslandCurb = .15f;
		static constexpr float kCanopyLength = 11.9f, kCanopyEave = 7.1f, kCanopyClearance = 5.0f;
		static constexpr float kExitUpstream = 22.0f, kExitDownstream = 10.5f;        ///< 出口アイランド 32.5m
		static constexpr float kEntranceUpstream = 36.0f, kEntranceDownstream = 12.5f; ///< 入口アイランド 48.5m
		/// @brief 料金所区間（車線が広がり切った区間）の長さと、その A 端から屋根の中心まで。
		/// 中央アイランドの両端をこの区間の両端に合わせ、前後のすり付け区間の分離帯へ続ける。
		static constexpr float kSectionLength = kExitUpstream + kEntranceUpstream, kCanopyAt = kExitUpstream;
		static constexpr float kOuterShoulder = 1.0f;
		Array<TollLane> lanes;
		Array<TollIsland> islands;
		float left = 0, right = 0;  ///< 舗装の両端（外側のアイランドの外縁）

		static const TollPlazaLayout& get()
		{
			static const TollPlazaLayout layout = []
			{
				TollPlazaLayout result;
				constexpr float w = kLaneWidth, island = kIslandWidth;
				const auto islandFor = [](bool exitSide, bool entranceSide, float left)
				{
					TollIsland value;
					value.left = left; value.width = kIslandWidth;
					value.from = -Max(exitSide ? kExitUpstream : 0.0f, entranceSide ? kEntranceDownstream : 0.0f);
					value.to = Max(exitSide ? kExitDownstream : 0.0f, entranceSide ? kEntranceUpstream : 0.0f);
					return value;
				};
				result.islands << islandFor(true, true, -island * .5f);
				// 出口: 中央から左へ ETC 専用・ETC/一般・一般。車両は ETC の 2 レーンを通る。
				float x = -island * .5f;
				const std::array<std::pair<TollLaneKind, int>, 3> exits{ { { TollLaneKind::EtcOnly, 1 }, { TollLaneKind::EtcGeneral, 0 }, { TollLaneKind::General, -1 } } };
				for (const auto& [kind, traffic] : exits)
				{
					x -= w;
					result.lanes << TollLane{ x, w, kind, true, traffic };
					x -= island;
					result.islands << islandFor(true, false, x);
				}
				result.left = x;
				// 入口: 中央から右へ ETC 専用・ETC/一般。B→A 向きの運転者の右が中央になる。
				x = island * .5f;
				const std::array<std::pair<TollLaneKind, int>, 2> entrances{ { { TollLaneKind::EtcOnly, 2 }, { TollLaneKind::EtcGeneral, 3 } } };
				for (const auto& [kind, traffic] : entrances)
				{
					result.lanes << TollLane{ x, w, kind, false, traffic };
					x += w;
					result.islands << islandFor(false, true, x);
					x += island;
				}
				result.right = x;
				return result;
			}();
			return layout;
		}

		/// @brief 通行車線 index（0, 1 = 出口の左・右、2, 3 = 入口の中央側・外側）のレーン。
		const TollLane& trafficLane(int index) const
		{
			for (const auto& lane : lanes) { if (lane.traffic == index) { return lane; } }
			return lanes.front();
		}
	};

	/// @brief 料金所前後の往復路の車線・部材の横位置（A→B 向きの左右）。すり付け区間の一端に使う。
	struct TollProfile
	{
		std::array<std::pair<float, float>, 4> lanes;  ///< 出口 2・入口 2 の [左, 右]
		float roadLeft = 0, roadRight = 0, medianHalf = 0, shoulder = 0;
	};

	inline TollProfile tollStemProfile()
	{
		const auto& c = GenerationSettings::get();
		const float width = c.expressway_rampLaneWidth, half = kTollSeparator * .5f;
		TollProfile profile;
		profile.lanes = { { { -half - 2 * width, -half - width }, { -half - width, -half }, { half, half + width }, { half + width, half + 2 * width } } };
		profile.roadLeft = -half - 2 * width; profile.roadRight = half + 2 * width;
		profile.medianHalf = half; profile.shoulder = c.expressway_rampOuterShoulder;
		return profile;
	}

	inline TollProfile tollPlazaProfile()
	{
		const auto& layout = TollPlazaLayout::get();
		TollProfile profile;
		for (int i = 0; i < 4; ++i)
		{
			const auto& lane = layout.trafficLane(i);
			profile.lanes[i] = { lane.left, lane.left + lane.width };
		}
		profile.roadLeft = layout.left; profile.roadRight = layout.right;
		profile.medianHalf = TollPlazaLayout::kIslandWidth * .5f; profile.shoulder = TollPlazaLayout::kOuterShoulder;
		return profile;
	}

	/// @brief 料金所の前後のすり付け区間。往復路（2+2）から料金所の幅へ広げる。plazaAtB: B 端が料金所側。
	/// @details 車線・路肩・防護柵の横位置を A 端と B 端で変え、区間内で直線的に移す。分離帯は中央アイランドへ続く。
	inline void applyTollTaper(RoadEdge& edge, bool plazaAtB)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const TollProfile stem = tollStemProfile(), plaza = tollPlazaProfile();
		const TollProfile& a = plazaAtB ? stem : plaza;
		const TollProfile& b = plazaAtB ? plaza : stem;
		edge.roadType = RoadType::Expressway;
		edge.speedLimit = 40;
		edge.lanes.clear();
		for (int i = 0; i < 4; ++i)
		{
			Lane value = lane(a.lanes[i].first, a.lanes[i].second - a.lanes[i].first, i < 2 ? LaneDir::Forward : LaneDir::Backward, LineType::None, LineType::None);
			value.offsetB_L = b.lanes[i].first; value.offsetB_R = b.lanes[i].second;
			edge.lanes << value;
		}
		const auto tapered = [](RoadPart part, float aLeft, float aRight, float bLeft, float bRight)
		{
			part.offsetA_L = aLeft; part.offsetA_R = aRight; part.offsetB_L = bLeft; part.offsetB_R = bRight;
			return part;
		};
		const float rail = c.expressway_guardrailWidth;
		edge.parts.clear();
		const auto add = [&](RoadPartType type, StringView definition, float aLeft, float aRight, float bLeft, float bRight)
		{
			edge.parts << tapered(strip(type, 0, 1, definition), aLeft, aRight, bLeft, bRight);
		};
		add(RoadPartType::Guardrail, U"guardrail_steel", a.roadLeft - a.shoulder - rail, a.roadLeft - a.shoulder, b.roadLeft - b.shoulder - rail, b.roadLeft - b.shoulder);
		add(RoadPartType::Shoulder, U"roadbed_asphalt", a.roadLeft - a.shoulder, a.roadLeft, b.roadLeft - b.shoulder, b.roadLeft);
		add(RoadPartType::Roadbed, U"roadbed_asphalt", a.roadLeft, -a.medianHalf, b.roadLeft, -b.medianHalf);
		add(RoadPartType::Median, U"median_concrete", -a.medianHalf, a.medianHalf, -b.medianHalf, b.medianHalf);
		add(RoadPartType::Roadbed, U"roadbed_asphalt", a.medianHalf, a.roadRight, b.medianHalf, b.roadRight);
		add(RoadPartType::Shoulder, U"roadbed_asphalt", a.roadRight, a.roadRight + a.shoulder, b.roadRight, b.roadRight + b.shoulder);
		add(RoadPartType::Guardrail, U"guardrail_steel", a.roadRight + a.shoulder, a.roadRight + a.shoulder + rail, b.roadRight + b.shoulder, b.roadRight + b.shoulder + rail);
		finish(edge);
	}

	/// @brief 料金所区間（アイランド・ブース・屋根の下）。全幅を舗装し、車線の線は引かない。
	/// @details 車両は ETC 専用・ETC/一般の各レーンを通る（4 車線）。一般レーンとアイランドは料金所の施設として描く。
	inline void applyTollPlaza(RoadEdge& edge)
	{
		using namespace detail;
		const auto& c = GenerationSettings::get();
		const TollProfile plaza = tollPlazaProfile();
		edge.roadType = RoadType::Expressway;
		// ETC 車線は 20km/h 以下で進入する（ETC システム利用規程）。
		edge.speedLimit = 20;
		edge.lanes.clear();
		for (int i = 0; i < 4; ++i) { edge.lanes << lane(plaza.lanes[i].first, plaza.lanes[i].second - plaza.lanes[i].first, i < 2 ? LaneDir::Forward : LaneDir::Backward, LineType::None, LineType::None); }
		const float rail = c.expressway_guardrailWidth;
		edge.parts.clear();
		edge.parts << strip(RoadPartType::Guardrail, plaza.roadLeft - plaza.shoulder - rail, rail, U"guardrail_steel");
		edge.parts << strip(RoadPartType::Shoulder, plaza.roadLeft - plaza.shoulder, plaza.shoulder, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Roadbed, plaza.roadLeft, plaza.roadRight - plaza.roadLeft, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Shoulder, plaza.roadRight, plaza.shoulder, U"roadbed_asphalt");
		edge.parts << strip(RoadPartType::Guardrail, plaza.roadRight + plaza.shoulder, rail, U"guardrail_steel");
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
