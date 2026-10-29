#include "RoadRenderer.hpp"
#include "MeshGeometry.hpp"
#include "../gen/GenerationSettings.hpp"
#include "../road/RoadGeometry.hpp"

using MeshGeometry::appendOrientedBox;

/// @file
/// @brief 高速道路の施設（料金所・分岐点滅灯）。形と配色は CSL の日本風料金所・道路テーマ MOD を簡略化したもの。

namespace
{
	/// @brief 施設ごとの材質。色は CSL 版の配色（淡い青緑の屋根、紫の ETC、緑の一般）に合わせる。
	enum FacilityMaterial : size_t { Canopy, Steel, Concrete, Booth, Etc, General, Housing, LampUpper, LampLower, Asphalt, Paint, CarLight, CarDark, CarRed, Wall, Roof, Glass, MaterialCount };

	void appendTollGate(std::array<MeshData, MaterialCount>& meshes, Vec3 base, Vec2 tangent, float width, float length, float height)
	{
		const Vec2 right{ tangent.y, -tangent.x };
		const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
		const auto at = [&](double along, double lateral, double up) { return Vec3{ base.x + tangent.x * along + right.x * lateral, base.y + up, base.z + tangent.y * along + right.y * lateral }; };
		// レーン数は屋根幅から決める（CSL 料金所の 5m 間隔）。分離島ごとにブースと柱を置く。
		const int lanes = Max(1, static_cast<int>(width / 5.0f));
		const double laneWidth = width / lanes;
		// 薄い大屋根と、前後の妻面に回した濃色の幕板（レーン表示板を取り付ける帯）。
		appendOrientedBox(meshes[Canopy], at(0, 0, height + .2), Float3{ length, .35f, width + 2.0f }, yaw);
		appendOrientedBox(meshes[Steel], at(0, 0, height - .1), Float3{ length * .97f, .25f, width + 1.6f }, yaw);
		for (const double end : { -1.0, 1.0 })
		{
			appendOrientedBox(meshes[Roof], at(end * (length * .5 + .05), 0, height - .45), Float3{ .12f, 1.3f, width + 2.0f }, yaw);
		}
		for (int island = 0; island <= lanes; ++island)
		{
			const double lateral = -width * .5 + laneWidth * island;
			// 分離島: 縁石の立ち上がりと、上流側の黄黒の衝撃吸収材。
			appendOrientedBox(meshes[Concrete], at(0, lateral, .16), Float3{ length * .86f, .32f, 1.2f }, yaw);
			appendOrientedBox(meshes[Housing], at(-length * .45, lateral, .55), Float3{ 1.4f, 1.1f, .9f }, yaw);
			appendOrientedBox(meshes[CarDark], at(-length * .45 - .35, lateral, .55), Float3{ .25f, 1.12f, .92f }, yaw);
			// 柱は島の上に立て、屋根の荷重を受ける。
			for (const double along : { -length * .32, length * .32 }) { appendOrientedBox(meshes[Steel], at(along, lateral, height * .5), Float3{ .38f, height, .38f }, yaw); }
			if (island == 0 || island == lanes) { continue; }
			// 料金収受ブースと、レーン入口の信号（ETC 車線の可否表示）。
			appendOrientedBox(meshes[Booth], at(-length * .05, lateral, 1.55), Float3{ 2.4f, 2.5f, 1.0f }, yaw);
			appendOrientedBox(meshes[Glass], at(-length * .05, lateral, 2.0), Float3{ 2.0f, 1.0f, 1.02f }, yaw);
			appendOrientedBox(meshes[CarDark], at(-length * .38, lateral, 1.6), Float3{ .3f, .6f, .3f }, yaw);
			appendOrientedBox(meshes[General], at(-length * .38 - .16, lateral, 1.72), Float3{ .03f, .18f, .18f }, yaw);
		}
		// 各レーンの入口側の幕板に表示板: 外側 2 レーンは一般（緑）、それ以外は ETC 専用（紫）。
		for (int lane = 0; lane < lanes; ++lane)
		{
			const double lateral = -width * .5 + laneWidth * (lane + .5);
			const bool general = lanes > 2 && (lane == 0 || lane == lanes - 1);
			appendOrientedBox(meshes[general ? General : Etc], at(-length * .5 - .14, lateral, height - .45), Float3{ .05f, .95f, static_cast<float>(laneWidth * .8) }, yaw);
			// ETC 路側アンテナ（CSL road_kit ETC20_DsrcAntenna）を各レーン上に吊る。
			appendOrientedBox(meshes[Steel], at(-length * .3, lateral, height - .7), Float3{ .45f, .3f, .7f }, yaw);
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
		// 衝撃吸収材。
		appendOrientedBox(meshes[Housing], at(-1.2, .45), Float3{ 1.8f, .9f, .9f }, yaw);
	}
}

Array<RoadRenderer::FacilityMesh> RoadRenderer::buildFacilityMeshes(const RoadNetwork& network, const RoadEdge& edge, const World& world) const
{
	const auto curve = network.getBezier(edge.id);
	if (!curve) { return {}; }
	const auto& settings = GenerationSettings::get();
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
			appendTollGate(meshes, base, tangent, settings.expressway_tollGateWidth * Max(object.scale, .2f), settings.expressway_tollGateLength, settings.expressway_tollGateHeight);
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
		ColorF{ .86, .82, .74 }, ColorF{ .30, .31, .33 }, ColorF{ .36, .48, .56 } };
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
