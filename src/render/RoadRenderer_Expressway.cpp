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
	enum FacilityMaterial : size_t { Canopy, Steel, Concrete, Booth, Etc, General, Housing, LampUpper, LampLower, MaterialCount };

	void appendTollGate(std::array<MeshData, MaterialCount>& meshes, Vec3 base, Vec2 tangent, float width, float length, float height)
	{
		const Vec2 right{ tangent.y, -tangent.x };
		const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
		const auto at = [&](double along, double lateral, double up) { return Vec3{ base.x + tangent.x * along + right.x * lateral, base.y + up, base.z + tangent.y * along + right.y * lateral }; };
		// 大屋根と庇、4 本の柱。
		appendOrientedBox(meshes[Canopy], at(0, 0, height), Float3{ length, .55f, width }, yaw);
		appendOrientedBox(meshes[Steel], at(0, 0, height - .42), Float3{ length * .96f, .3f, width * .96f }, yaw);
		for (const double along : { -length * .38, length * .38 })
		{
			for (const double lateral : { -width * .42, width * .42 })
			{
				appendOrientedBox(meshes[Steel], at(along, lateral, height * .5), Float3{ .45f, height, .45f }, yaw);
			}
		}
		// レーン両側の分離島と料金収受ブース。遮断棒は上げた状態。
		for (const double lateral : { -2.6, 2.6 })
		{
			appendOrientedBox(meshes[Concrete], at(0, lateral, .14), Float3{ length * .8f, .28f, 1.3f }, yaw);
			appendOrientedBox(meshes[Booth], at(-length * .08, lateral, 1.45), Float3{ 2.2f, 2.3f, 1.1f }, yaw);
			appendOrientedBox(meshes[Steel], at(length * .22, lateral, 1.7), Float3{ .12f, 3.0f, .12f }, yaw);
		}
		// 入口正面の案内板（左: ETC 専用の紫、右: 一般の緑）。
		appendOrientedBox(meshes[Etc], at(-length * .5 - .05, -width * .22, height - 1.1), Float3{ .08f, .95f, width * .38f }, yaw);
		appendOrientedBox(meshes[General], at(-length * .5 - .05, width * .22, height - 1.1), Float3{ .08f, .95f, width * .38f }, yaw);
		// ETC 路側アンテナ（CSL road_kit ETC20_DsrcAntenna）。
		appendOrientedBox(meshes[Steel], at(-length * .3, 0, height - 1.0), Float3{ .5f, .35f, .9f }, yaw);
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
		if (object.id < 0 || object.parentEdgeId != edge.id || (object.type != RoadObjectType::TollGate && object.type != RoadObjectType::DivergeBlinker)) { continue; }
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
			appendTollGate(meshes, base, tangent, settings.expressway_tollGateWidth, settings.expressway_tollGateLength, settings.expressway_tollGateHeight);
		}
		else { appendBlinker(meshes, base, tangent); }
	}
	static constexpr std::array<ColorF, MaterialCount> kColors{
		ColorF{ .80, .90, .88 }, ColorF{ .58, .60, .60 }, ColorF{ .66, .66, .62 }, ColorF{ .92, .92, .90 },
		ColorF{ .42, .20, .55 }, ColorF{ .10, .50, .26 }, ColorF{ .73, .50, .06 }, ColorF{ 1 }, ColorF{ 1 } };
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
