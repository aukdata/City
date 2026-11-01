#include "RoadRenderer.hpp"
#include "MeshGeometry.hpp"
#include "../gen/ExpresswayCrossSection.hpp"
#include "../gen/GenerationSettings.hpp"
#include "../road/RoadGeometry.hpp"

using MeshGeometry::appendOrientedBox;

/// @file
/// @brief 高速道路の施設（料金所・分岐点滅灯・PA/SA）。料金所は NEXCO 設計要領の寸法、点滅灯は CSL の道路テーマ MOD を簡略化したもの。

namespace
{
	/// @brief 施設ごとの材質。
	enum FacilityMaterial : size_t { Canopy, Steel, Concrete, Booth, Etc, General, Housing, LampUpper, LampLower, Asphalt, Paint, CarLight, CarDark, CarRed, Wall, Roof, Glass, CushionRed, CushionWhite,
		Fascia, Soffit, Downlight, IslandTop, Yellow, Black, SignalGreen, MaterialCount };

	/// @brief IC の料金所（NEXCO 設計要領 第六集の寸法）。base は屋根の中心の路面、tangent は連絡路の A→B（出口の進行方向）。
	/// @details アイランド・ブース・発進制御機（開閉バー）・路側表示器・ETC アンテナ・屋根（幕板と天井の照明）・
	///   高所照明・料金所事務所を置く。車線表示板（ETC 専用など）は案内標識として幕板の前に付く。
	void appendTollGate(std::array<MeshData, MaterialCount>& meshes, Vec3 base, Vec2 tangent)
	{
		using Toll = ExpresswayCrossSection::TollPlazaLayout;
		const auto& layout = Toll::get();
		const Vec2 right{ tangent.y, -tangent.x };
		const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
		const auto at = [&](double along, double lateral, double up) { return Vec3{ base.x + tangent.x * along + right.x * lateral, base.y + up, base.z + tangent.y * along + right.y * lateral }; };
		const auto box = [&](size_t material, double along, double lateral, double bottom, double length, double height, double width)
		{
			appendOrientedBox(meshes[material], at(along, lateral, bottom + height * .5), Float3{ static_cast<float>(length), static_cast<float>(height), static_cast<float>(width) }, yaw);
		};
		constexpr double curb = Toll::kIslandCurb + .05, clearance = Toll::kCanopyClearance, halfCanopy = Toll::kCanopyLength * .5;
		constexpr double fasciaHeight = 1.5, roofTop = clearance + fasciaHeight;
		// 屋根の長辺の軒は、最も外のアイランドの中心から 7.1m。
		double outerLeft = Math::Inf, outerRight = -Math::Inf;
		for (const auto& island : layout.islands)
		{
			outerLeft = Min(outerLeft, static_cast<double>(island.left + island.width * .5f));
			outerRight = Max(outerRight, static_cast<double>(island.left + island.width * .5f));
		}
		const double canopyLeft = outerLeft - Toll::kCanopyEave, canopyRight = outerRight + Toll::kCanopyEave;
		const double canopyWidth = canopyRight - canopyLeft, canopyMid = (canopyLeft + canopyRight) * .5;
		const auto islandAt = [&](float edge, bool leftEdge) -> const ExpresswayCrossSection::TollIsland*
		{
			for (const auto& island : layout.islands) { if (Abs((leftEdge ? island.left : island.left + island.width) - edge) < .01f) { return &island; } }
			return nullptr;
		};

		// ---- アイランド: 縁石で囲ったコンクリートの島。進入側の鼻に黄黒の衝撃緩衝材と黄色の防護ポール ----
		for (const auto& island : layout.islands)
		{
			const double middle = island.left + island.width * .5, length = island.to - island.from;
			box(Concrete, (island.from + island.to) * .5, middle, 0, length, curb, island.width);
			box(IslandTop, (island.from + island.to) * .5, middle, curb, length - .3, .01, island.width - .3);
			const bool center = island.from <= -Toll::kExitUpstream + .01 && island.to >= Toll::kEntranceUpstream - .01;
			if (center) { continue; }
			for (const bool upstreamIsNegative : { true, false })
			{
				// 出口レーンに接する島は A 側の端、入口レーンに接する島は B 側の端が進入側。
				const bool facesExit = island.from <= -Toll::kExitUpstream + .01, facesEntrance = island.to >= Toll::kEntranceUpstream - .01;
				if (upstreamIsNegative ? !facesExit : !facesEntrance) { continue; }
				const double sign = upstreamIsNegative ? -1.0 : 1.0, nose = upstreamIsNegative ? island.from : island.to;
				for (int band = 0; band < 4; ++band)
				{
					box(band % 2 == 0 ? Yellow : Black, nose + sign * (.15 + band * .3), middle, 0, .3, 1.0, island.width * .8);
				}
				for (int post = 0; post < 2; ++post) { box(Yellow, nose - sign * (1.0 + post * 1.2), middle, curb, .16, .9, .16); }
			}
		}

		// ---- レーンごとの設備 ----
		for (const auto& lane : layout.lanes)
		{
			// 進行方向（along の向き）と、運転者の右のアイランド（ブースを置く）。
			const double travel = lane.exit ? 1.0 : -1.0, middle = lane.left + lane.width * .5;
			const auto* boothIsland = lane.exit ? islandAt(lane.left + lane.width, true) : islandAt(lane.left, false);
			const auto* farIsland = lane.exit ? islandAt(lane.left, false) : islandAt(lane.left + lane.width, true);
			const bool etc = lane.kind != ExpresswayCrossSection::TollLaneKind::General;
			if (boothIsland)
			{
				const double islandMiddle = boothIsland->left + boothIsland->width * .5;
				// 料金収受ブース 2.8 × 1.6 × 2.5m: 白い箱に、レーン側と前後の窓、軒の出た屋根。
				box(Booth, 0, islandMiddle, curb, 2.8, 2.5, 1.6);
				box(Glass, 0, islandMiddle, curb + 1.0, 2.5, 1.1, 1.64);
				box(Glass, 0, islandMiddle, curb + 1.0, 2.84, 1.1, 1.2);
				box(Roof, 0, islandMiddle, curb + 2.5, 3.2, .18, 2.0);
				// 路側表示器（「↑ETC」を示す表示板）: ブースの手前の島の上、運転者の目の高さ。
				if (etc)
				{
					const double display = -travel * 7.0;
					box(Steel, display, islandMiddle, curb, .1, 1.2, .1);
					box(CarDark, display, islandMiddle, curb + 1.2, .2, .45, .6);
					box(SignalGreen, display - travel * .11, islandMiddle, curb + 1.3, .02, .25, .45);
					// 発進制御機（開閉バー）: ブースの先に筐体、腕は黄黒の縞で車線を塞ぐ。
					const double gate = travel * 2.6;
					box(Housing, gate, islandMiddle, curb, .5, 1.05, .45);
					const double armStart = lane.exit ? lane.left + lane.width : lane.left;
					const double direction = lane.exit ? -1.0 : 1.0;
					for (int stripe = 0; stripe < 6; ++stripe)
					{
						const double from = armStart + direction * (.1 + stripe * .45);
						box(stripe % 2 == 0 ? Yellow : Black, gate, from + direction * .225, .85, .08, .1, .45);
					}
				}
			}
			// 一般・ETC/一般レーンはブースの横に停止線。
			if (lane.kind != ExpresswayCrossSection::TollLaneKind::EtcOnly) { box(Paint, -travel * 1.6, middle, .02, .45, .02, lane.width); }
			// 車線の入口の信号（緑の↓）を島の鼻の柱に。
			if (farIsland)
			{
				const double nose = lane.exit ? -Toll::kExitUpstream + 4.0 : Toll::kEntranceUpstream - 4.0;
				const double post = farIsland->left + farIsland->width * .5;
				box(Steel, nose, post, curb, .14, 2.8, .14);
				box(CarDark, nose, post, curb + 2.6, .3, .55, .55);
				box(SignalGreen, nose - travel * .16, post, curb + 2.7, .02, .35, .35);
			}
			// ETC 路側アンテナ: 屋根の進入側の梁から車線の上に吊る（一般レーンはなし）。
			if (etc)
			{
				const double antenna = -travel * (halfCanopy - .8);
				box(Soffit, antenna, middle, clearance - .3, .35, .5, .7);
			}
		}

		// ---- 屋根: 幕板で囲った平屋根と、照明を並べた天井。柱は各アイランドに 2 本 ----
		box(Roof, 0, canopyMid, roofTop - .25, Toll::kCanopyLength, .25, canopyWidth);
		box(Soffit, 0, canopyMid, clearance + .2, Toll::kCanopyLength - .3, .08, canopyWidth - .3);
		for (const double end : { -1.0, 1.0 })
		{
			box(Fascia, end * (halfCanopy - .08), canopyMid, clearance, .16, fasciaHeight, canopyWidth);
			box(Steel, end * (halfCanopy - .06), canopyMid, roofTop - .32, .2, .14, canopyWidth + .02);
			const double side = end < 0 ? canopyLeft : canopyRight;
			box(Fascia, 0, side - end * .08, clearance, Toll::kCanopyLength, fasciaHeight, .16);
		}
		for (double lateral = canopyLeft + 1.5; lateral < canopyRight - 1.0; lateral += 3.0)
		{
			for (const double along : { -3.0, 0.0, 3.0 }) { box(Downlight, along, lateral, clearance + .14, .9, .06, .9); }
		}
		for (const auto& island : layout.islands)
		{
			const double middle = island.left + island.width * .5;
			for (const double along : { -4.8, 4.8 }) { box(Steel, along, middle, curb, .45, clearance + .2 - curb, .45); }
		}

		// ---- 広場の高所照明（料金所幅の半分以上の高さ）: 両側の防護柵の外 ----
		const double mast = Max(15.0, (layout.right - layout.left) * .55);
		for (const auto& [lateral, along] : { std::pair{ static_cast<double>(layout.left) - Toll::kOuterShoulder - 2.0, -16.0 }, std::pair{ static_cast<double>(layout.right) + Toll::kOuterShoulder + 2.0, 24.0 } })
		{
			box(Steel, along, lateral, -3.0, .6, mast + 3.0, .6);
			box(Housing, along, lateral, mast - .4, 2.4, .6, 2.4);
			box(Downlight, along, lateral, mast - .45, 2.0, .06, 2.0);
		}

		// ---- 料金所事務所（出口側）: 車線端の縁石から 5m 以上離した 2 階建て、前に駐車場 ----
		const double officeNear = layout.left - Toll::kOuterShoulder - 6.0, officeDepth = 11.0, officeLength = 22.0, officeAlong = Toll::kExitDownstream + officeLength * .5 + 2.0;
		const double officeMiddle = officeNear - officeDepth * .5;
		box(Concrete, officeAlong - 6.0, officeMiddle - 2.0, -4.0, officeLength + 20.0, 4.05, officeDepth + 10.0);
		box(Wall, officeAlong, officeMiddle, 0, officeLength, 7.0, officeDepth);
		for (const double floor : { 1.0, 4.4 })
		{
			box(Glass, officeAlong, officeMiddle, floor, officeLength - 1.5, 1.4, officeDepth + .06);
			box(Glass, officeAlong, officeMiddle, floor, officeLength + .06, 1.4, officeDepth - 1.5);
		}
		box(Roof, officeAlong, officeMiddle, 7.0, officeLength + .6, .4, officeDepth + .6);
		box(Asphalt, officeAlong - officeLength * .5 - 7.0, officeMiddle, .05, 12.0, .03, officeDepth + 6.0);
		for (int stall = 0; stall < 5; ++stall)
		{
			box(Paint, officeAlong - officeLength * .5 - 12.0 + stall * 2.5, officeMiddle, .08, .12, .02, 5.0);
		}
	}

	/// @brief PA/SA: 本線側の駐車場（区画線と駐車車両）と、通路の外側のトイレ・売店棟。
	/// @param padTop 駐車場の舗装面の高さ。造成した盛土として下へ厚みを持たせ、地形に埋もれないようにする。
	void appendServiceArea(std::array<MeshData, MaterialCount>& meshes, Vec3 base, Vec2 tangent, double length, double size, int seed, double padTop)
	{
		const Vec2 right{ tangent.y, -tangent.x };
		const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
		const auto at = [&](double along, double lateral, double up) { return Vec3{ base.x + tangent.x * along + right.x * lateral, base.y + up, base.z + tangent.y * along + right.y * lateral }; };
		const double padLength = length * .82, padNear = 4.0, padWidth = 30.0;
		constexpr double kPadDepth = 4.0;
		const double lift = padTop - base.y;
		appendOrientedBox(meshes[Asphalt], at(0, padNear + padWidth * .5, lift - kPadDepth * .5 + .03), Float3{ static_cast<float>(padLength), static_cast<float>(kPadDepth), static_cast<float>(padWidth) }, yaw);
		// 2 列の駐車区画（2.5m 間隔）。車両の有無と色は位置から決める。
		for (const double row : { padNear + 6.0, padNear + padWidth - 6.0 })
		{
			for (double along = -padLength * .5 + 2; along < padLength * .5 - 2; along += 2.5)
			{
				appendOrientedBox(meshes[Paint], at(along, row, lift + .06), Float3{ .12f, .02f, 5.0f }, yaw);
				const uint32 hash = static_cast<uint32>(seed * 73856093) ^ static_cast<uint32>(static_cast<int>(along * 4) * 19349663) ^ static_cast<uint32>(static_cast<int>(row) * 83492791);
				if (hash % 10 < 3) { continue; }
				const size_t colour = hash % 3 == 0 ? CarDark : hash % 3 == 1 ? CarLight : CarRed;
				appendOrientedBox(meshes[colour], at(along + 1.25, row, lift + .48), Float3{ 1.7f, .72f, 4.3f }, yaw);
				appendOrientedBox(meshes[colour], at(along + 1.25, row - .2, lift + 1.08), Float3{ 1.5f, .5f, 2.2f }, yaw);
				appendOrientedBox(meshes[Glass], at(along + 1.25, row - .2, lift + 1.1), Float3{ 1.52f, .36f, 2.24f }, yaw);
			}
		}
		// 通路の外側（左）に休憩施設棟。SA は売店・食堂を含む大きい棟。
		const double width = 36 * size, depth = 14 * size;
		appendOrientedBox(meshes[Wall], at(0, -(8 + depth * .5), 2.6), Float3{ static_cast<float>(width), 5.2f, static_cast<float>(depth) }, yaw);
		appendOrientedBox(meshes[Glass], at(0, -8.05, 1.8), Float3{ static_cast<float>(width * .8), 2.6f, .1f }, yaw);
		appendOrientedBox(meshes[Roof], at(0, -(8 + depth * .5), 5.45), Float3{ static_cast<float>(width + 3), .5f, static_cast<float>(depth + 4) }, yaw);
		appendOrientedBox(meshes[Asphalt], at(0, -(4 + depth * .25), .03), Float3{ static_cast<float>(width + 12), .05f, static_cast<float>(depth * .5 + 8) }, yaw);
	}

	void appendBlinker(std::array<MeshData, MaterialCount>& meshes, Vec3 base, Vec2 tangent)
	{
		const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
		const auto at = [&](double along, double up) { return Vec3{ base.x + tangent.x * along, base.y + up, base.z + tangent.y * along }; };
		MeshGeometry::appendMeshData(meshes[Steel], MeshData::Cylinder(Float3{ at(0, 1.1) }, .065, 2.2, 10));
		appendOrientedBox(meshes[Housing], at(0, 2.18), Float3{ .22f, .87f, .48f }, yaw);
		// 2 眼の灯器は上流（接近車両）側を向く。
		appendOrientedBox(meshes[LampUpper], at(-.13, 2.4), Float3{ .04f, .2f, .2f }, yaw);
		appendOrientedBox(meshes[LampLower], at(-.13, 1.97), Float3{ .04f, .2f, .2f }, yaw);
		// 分流鼻のクッションドラム: 赤白の帯を巻いた樹脂製の円筒を 2 列に並べる。
		const Vec2 right{ tangent.y, -tangent.x };
		for (int row = 0; row < 3; ++row)
		{
			for (const double side : { -.5, .5 })
			{
				if (row == 0 && side > 0) { continue; }
				const double along = -1.1 - row * 1.05, lateral = row == 0 ? 0.0 : side;
				const Vec3 drum{ base.x + tangent.x * along + right.x * lateral, base.y, base.z + tangent.y * along + right.y * lateral };
				for (int band = 0; band < 4; ++band)
				{
					MeshGeometry::appendMeshData(meshes[band % 2 == 0 ? CushionRed : CushionWhite], MeshData::Cylinder(Float3{ drum + Vec3{ 0, .12 + band * .225, 0 } }, .45, .225, 16));
				}
			}
		}
	}
}

Array<RoadRenderer::FacilityMesh> RoadRenderer::buildFacilityMeshes(const RoadNetwork& network, const RoadEdge& edge, const World& world) const
{
	const auto curve = network.getBezier(edge.id);
	if (!curve) { return {}; }
	std::array<MeshData, MaterialCount> meshes;
	for (const auto& object : network.objects())
	{
		if (object.id < 0 || object.parentEdgeId != edge.id
			|| (object.type != RoadObjectType::TollGate && object.type != RoadObjectType::DivergeBlinker && object.type != RoadObjectType::ServiceArea)) { continue; }
		const float arc = Clamp(object.arcPos, 0.0f, curve->totalLength);
		const Vec3 tangent3 = curve->tangentAt(arc);
		Vec2 tangent{ tangent3.x, tangent3.z };
		if (tangent.lengthSq() < 1e-9) { continue; }
		tangent.normalize();
		Vec3 base = curve->positionAt(arc) + tangentToRight(tangent3) * object.lateralOffset;
		const double terrain = world.sampleHeight(static_cast<float>(base.x), static_cast<float>(base.z));
		base.y = RoadGeometry::furnitureBaseY(edge, curve->positionAt(arc), edge.usesDesignHeight() ? curve->positionAt(arc).y : terrain);
		if (object.type == RoadObjectType::TollGate)
		{
			appendTollGate(meshes, base, tangent);
		}
		else if (object.type == RoadObjectType::ServiceArea)
		{
			// 舗装面は通路の高さと、駐車場の範囲で最も高い地盤のうち高い方（最大 1.5m 持ち上げ）。
			const Vec2 side{ tangent.y, -tangent.x };
			double ground = base.y;
			for (double along = -curve->totalLength * .4; along <= curve->totalLength * .4; along += 12)
			{
				for (const double lateral : { 6.0, 18.0, 32.0 })
				{
					ground = Max(ground, static_cast<double>(world.sampleHeight(static_cast<float>(base.x + tangent.x * along + side.x * lateral), static_cast<float>(base.z + tangent.y * along + side.y * lateral))));
				}
			}
			appendServiceArea(meshes, base, tangent, curve->totalLength, object.scale, object.id, Min(ground + .05, base.y + 1.5));
		}
		else { appendBlinker(meshes, base, tangent); }
	}
	static constexpr std::array<ColorF, MaterialCount> kColors{
		ColorF{ .80, .90, .88 }, ColorF{ .58, .60, .60 }, ColorF{ .66, .66, .62 }, ColorF{ .92, .92, .90 },
		ColorF{ .42, .20, .55 }, ColorF{ .10, .50, .26 }, ColorF{ .73, .50, .06 }, ColorF{ 1 }, ColorF{ 1 },
		ColorF{ .19, .19, .20 }, ColorF{ .92, .92, .90 }, ColorF{ .86, .87, .88 }, ColorF{ .12, .13, .16 }, ColorF{ .58, .12, .10 },
		ColorF{ .86, .82, .74 }, ColorF{ .30, .31, .33 }, ColorF{ .36, .48, .56 }, ColorF{ .78, .10, .08 }, ColorF{ .93, .93, .91 },
		ColorF{ .94, .94, .92 }, ColorF{ .78, .79, .78 }, ColorF{ 1.0, .98, .90 }, ColorF{ .72, .72, .69 }, ColorF{ .95, .76, .05 }, ColorF{ .08, .08, .09 }, ColorF{ .15, .95, .45 } };
	Array<FacilityMesh> result;
	for (size_t i = 0; i < meshes.size(); ++i)
	{
		if (meshes[i].indices.isEmpty()) { continue; }
		const int phase = i == LampUpper ? 0 : i == LampLower ? 1 : -1;
		result << FacilityMesh{ Mesh{ meshes[i] }, kColors[i].removeSRGBCurve(), phase };
	}
	return result;
}

void RoadRenderer::drawFacilities(const RoadNetwork& network, const RoadEdge& edge, const World& world)
{
	auto found = m_facilityMeshCache.find(edge.id);
	if (found == m_facilityMeshCache.end())
	{
		if (!canBuildCache()) { return; }
		found = m_facilityMeshCache.emplace(edge.id, buildFacilityMeshes(network, edge, world)).first;
	}
	// 分岐点滅灯は上下の灯器を 0.5 秒ごとに交互点灯する（CSL AnimateBlinkers）。
	const int lit = static_cast<int>(Math::Floor(Scene::Time() / .5)) & 1;
	for (const auto& facility : found->second)
	{
		if (facility.blinkPhase < 0) { facility.mesh.draw(facility.color); continue; }
		facility.mesh.draw(facility.blinkPhase == lit ? ColorF{ 1.0, .57, .025 }.removeSRGBCurve() : ColorF{ .10, .055, .012 }.removeSRGBCurve());
	}
}
