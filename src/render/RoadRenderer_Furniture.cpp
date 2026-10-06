#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "MeshGeometry.hpp"
#include "MountainRoadGeometry.hpp"
#include "../road/RoadEnvironment.hpp"

using namespace RoadRenderDetail;
using MeshGeometry::appendOrientedBox;

/// @file
/// @brief 電柱・沿道設備・排水設備の構築。

namespace
{
	/// @brief At acute merges, an approach sidewalk must not continue inside its
	/// neighbour's carriageway beyond the nominal junction cutoff.
	/// @brief CPU cylinders keep round utility hardware in the existing material batches.
	void appendCylinder(MeshData& mesh, Vec3 center, double radius, double height, uint32 sides = 12)
	{
		auto cylinder = MeshData::Cylinder(Float3{center}, radius, height, sides);
		appendMeshData(mesh,cylinder);
	}

}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildStreetFurnitureBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const World& world,
	float marginA, float marginB) const
{
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	MeshData poleMd;
	MeshData cableMd;
	MeshData roadsideMd;
	MeshData markerMd;
	MeshData ironMd;
	const auto mountain=MountainRoadGeometry::build(edge,bezier,world,range.sStart,range.sEnd);

	for (size_t partIndex = 0; partIndex < edge.parts.size(); ++partIndex)
	{
		const RoadPart& part = edge.parts[partIndex];
		if (part.build != BuildState::Built || part.placement != RoadPartPlacement::RepeatAlongEdge)
		{
			continue;
		}

		const float offset = (part.offsetL() + part.offsetR()) * 0.5f;
		float spacing = part.repeatSpacing;
		float jitter = part.repeatJitter;
		if (!part.defId.isEmpty())
		{
			const RoadPartDef& def = m_partRegistry.get(part.defId);
			if (part.useDefinitionRepeatSpacing) spacing = def.repeatSpacing;
			if (part.useDefinitionRepeatJitter) jitter = def.repeatJitter;
		}
		spacing = Max(spacing, 4.0f);
		jitter = Max(jitter, 0.0f);
		const float objectHalfDepth = (part.type == RoadPartType::UtilityPole) ? 0.12f : 0.35f;
		const float sStart = range.sStart + spacing * 0.5f;
		const float sEnd = range.sEnd - spacing * 0.25f;
		if (sEnd <= sStart) continue;

		Optional<Vec3> previousPole;
		float previousArc = sStart;
		Vec2 previousRight{ 0, 0 };
		const int sampleCount = Max(1, static_cast<int>((sEnd - sStart) / spacing));
		for (int i = 0; i <= sampleCount; ++i)
		{
			const float jitterOffset = (jitter > 0.0f)
				? (static_cast<float>(((edge.id * 73856093) ^ (static_cast<int>(partIndex) * 19349663) ^ (i * 83492791)) & 1023) / 1023.0f - 0.5f) * jitter
				: 0.0f;
			const float s = Clamp(sStart + static_cast<float>(i) * spacing + jitterOffset, range.sStart, range.sEnd);
			const Vec3 pos = bezier.positionAt(s);
			if (!RoadEnvironment::outdoorSpan(world,bezier,previousArc,s,edge.useElevation || edge.tunnel)) { previousPole.reset(); }
			previousArc=s;
			if (RoadEnvironment::coveredAt(world,pos,edge.useElevation || edge.tunnel)) { continue; }
			const Vec3 tan3 = bezier.tangentAt(s);
			Vec2 tangent{ static_cast<float>(tan3.x), static_cast<float>(tan3.z) };
			if (tangent.lengthSq() <= 1e-8f) continue;
			tangent.normalize();
			const Vec2 right{ tangent.y, -tangent.x };
			const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
			const double anchorX = pos.x + right.x * static_cast<double>(offset);
			const double anchorZ = pos.z + right.y * static_cast<double>(offset);
			const double terrainY = edge.usesDesignHeight()
				? pos.y
				: world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z));
			const float groundY = static_cast<float>(RoadGeometry::furnitureBaseY(edge, pos, terrainY));

			if (part.type == RoadPartType::UtilityPole)
			{
				const Vec3 poleCenter{ anchorX, groundY + 5.1f, anchorZ };
				const float armYaw = yaw + static_cast<float>(Math::HalfPi);
				auto shaft = MeshData::Cylinder(Float3{poleCenter},0.18,10.2,12);
				for (auto& vertex : shaft.vertices)
				{
					const float taper = Math::Lerp(1.0f,0.6f,Clamp((vertex.pos.y-groundY)/10.2f,0.0f,1.0f));
					vertex.pos.x = static_cast<float>(anchorX + (vertex.pos.x-anchorX)*taper);
					vertex.pos.z = static_cast<float>(anchorZ + (vertex.pos.z-anchorZ)*taper);
				}
				appendMeshData(poleMd,shaft);
				for (const double level : { 3.6, 2.8 })
				{
					appendOrientedBox(roadsideMd,poleCenter+Vec3{0,level,0},Float3{1.9f,0.10f,0.13f},armYaw);
					appendCylinder(roadsideMd,poleCenter+Vec3{0,level,0},0.19,0.12);
					for (const double armOffset : { -0.75,0.0,0.75 })
					{
						const Vec3 anchor = poleCenter + Vec3{right.x*armOffset,level+.18,right.y*armOffset};
						appendCylinder(markerMd,anchor,.055,.32,8);
						for (const double ring : { -.10,0.0,.10 }) { appendCylinder(markerMd,anchor+Vec3{0,ring,0},.10,.055,8); }
					}
				}
				// Pole-mounted transformer, mounting straps, bushings and service conduit.
				const Vec3 transformer = poleCenter + Vec3{-right.x*.43,1.65,-right.y*.43};
				appendCylinder(roadsideMd,transformer,.27,.92);
				appendCylinder(roadsideMd,transformer+Vec3{0,.49,0},.31,.07);
				appendCylinder(markerMd,transformer+Vec3{0,.63,0},.065,.22,8);
				appendOrientedBox(roadsideMd,poleCenter+Vec3{-right.x*.20,1.55,-right.y*.20},Float3{.65f,.09f,.15f},armYaw);
				appendCylinder(roadsideMd,Vec3{anchorX+right.x*.19,groundY+2.5,anchorZ+right.y*.19},.022,4.6,6);
				appendOrientedBox(markerMd,Vec3{anchorX,groundY+1.3,anchorZ},Float3{.23f,.35f,.23f},yaw);
				for (int rung = 0; rung < 11; ++rung)
				{
					const float side = rung%2 == 0 ? -1.0f : 1.0f;
					appendOrientedBox(roadsideMd,Vec3{anchorX+right.x*.22*side,groundY+2.1+rung*.52,anchorZ+right.y*.22*side},Float3{.23f,.032f,.035f},armYaw);
				}
				if (previousPole)
				{
					for (const float cableSide : { -0.75f, 0.0f, 0.75f })
					{
						const Vec3 from = *previousPole + Vec3{ previousRight.x * cableSide, 3.95, previousRight.y * cableSide };
						const Vec3 to = poleCenter + Vec3{ right.x * cableSide, 3.95, right.y * cableSide };
						const double sag = Min(0.65, from.distanceFrom(to) * 0.015);
						constexpr int kCableSegments = 6;
						Vec3 previous = from;
						for (int segment = 1; segment <= kCableSegments; ++segment)
						{
							const double fraction = static_cast<double>(segment) / kCableSegments;
							const Vec3 current = from.lerp(to, fraction) + Vec3{ 0, -4.0 * sag * fraction * (1.0 - fraction), 0 };
							const Vec3 delta = current - previous;
							const double length = delta.length();
							if (length > 0.001)
							{
								const uint32 base = static_cast<uint32>(cableMd.vertices.size());
								const Float3 direction{ delta / length };
								for (const Vec3 endpoint : { previous, current })
								{
									for (const float side : { -1.0f, 1.0f })
									{
										cableMd.vertices << Vertex3D{ Float3{ endpoint }, direction, Float2{ side, 0.014f } };
									}
								}
								cableMd.indices << TriangleIndex32{ base, base + 1, base + 2 } << TriangleIndex32{ base + 2, base + 1, base + 3 };
							}
							previous = current;
						}
					}
				}
				previousPole = poleCenter;
				previousRight = right;
			}
			else if (part.type == RoadPartType::RoadsideObject)
			{
				const Vec3 center{ anchorX, groundY + 0.85f, anchorZ };
				appendOrientedBox(roadsideMd, center, Float3{ 0.45f, 1.70f, objectHalfDepth * 2.0f }, yaw);
			}
		}
	}

	// 排水桝と点検蓋は交差点の切り詰め範囲を避け、道路の地表面に沿わせる。
	if (!edge.useElevation)
	{
		constexpr float kDrainSpacing = 18.0f;
		for (const auto& part : edge.parts)
		{
			if (part.type != RoadPartType::Sidewalk || part.build != BuildState::Built) { continue; }
			const float inner = Abs(part.offsetL()) < Abs(part.offsetR()) ? part.offsetL() : part.offsetR();
			const float offset = inner + (inner < 0 ? -.22f : .22f);
			for (float arc = range.sStart + 8; arc < range.sEnd - 8; arc += kDrainSpacing)
			{
				const Vec3 tangent = bezier.tangentAt(arc);
				const Vec3 right = tangentToRight(tangent);
				Vec3 center = bezier.positionAt(arc) + right * offset;
				center.y = makeSlice(bezier,world,arc,kRoadSurfaceLift,edge.usesDesignHeight()).center.y + .155;
				const float yaw = static_cast<float>(Atan2(tangent.z, tangent.x));
				appendOrientedBox(roadsideMd, center, Float3{.70f,.018f,.35f}, yaw);
				for (int bar = -4; bar <= 4; ++bar)
				{
					appendOrientedBox(ironMd, center + tangent * (bar * .072) + Vec3{0,.012,0}, Float3{.022f,.016f,.32f}, yaw);
				}
			}
		}
		constexpr float kCoverSpacing = 47.0f;
		for (float arc = range.sStart + 15; arc < range.sEnd - 12; arc += kCoverSpacing)
		{
			const Vec3 tangent = bezier.tangentAt(arc);
			const Vec3 right = tangentToRight(tangent);
			Vec3 center = bezier.positionAt(arc) + right * .6;
			center.y = makeSlice(bezier,world,arc,kRoadSurfaceLift,edge.usesDesignHeight()).center.y + .012;
			appendCylinder(roadsideMd, center, .32, .012, 24);
			for (int rib = -2; rib <= 2; ++rib)
			{
				appendOrientedBox(ironMd, center + right * (rib * .09) + Vec3{0,.01,0}, Float3{.018f,.006f,.43f}, static_cast<float>(Atan2(right.z,right.x)));
			}
		}
	}
	Array<LaneLineBatch> batches;
	for (const auto& item : {std::pair<const MeshData*,ColorF>{&mountain.wall,ColorF{.43,.45,.40}},
		{&mountain.moss,ColorF{.24,.34,.16}},{&mountain.rail,ColorF{.48,.49,.43}},
		{&mountain.posts,ColorF{.82,.81,.69}},{&mountain.reflectors,ColorF{1,.36,.07}}})
	{
		if (!item.first->indices.isEmpty()) { batches<<LaneLineBatch{item.second.removeSRGBCurve(),Mesh{*item.first}}; }
	}
	if (!poleMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.57, 0.57, 0.53 }.removeSRGBCurve(), Mesh{ poleMd } };
	if (!cableMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.05, 0.05, 0.045 }.removeSRGBCurve(), Mesh{ cableMd }, true };
	if (!markerMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.82, 0.78, 0.64 }.removeSRGBCurve(), Mesh{ markerMd } };
	if (!ironMd.vertices.isEmpty()) { batches << LaneLineBatch{ColorF{.43,.44,.43}.removeSRGBCurve(),Mesh{ironMd}}; }
	if (!roadsideMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.30, 0.33, 0.33 }.removeSRGBCurve(), Mesh{ roadsideMd } };
	return batches;
}
