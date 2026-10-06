#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "../road/JunctionGeometry.hpp"

using namespace RoadRenderDetail;

/// @file
/// @brief 道路部品・路面・交差点の CPU メッシュ構築。

namespace
{
	float roadPartSideSkirtDrop(RoadPartType type, float heightOffset)
	{
		switch (type)
		{
		case RoadPartType::Curb:
			return Max(0.035f, heightOffset + 0.010f);
		case RoadPartType::Roadbed:
			return Max(0.180f, heightOffset + 0.045f);
		case RoadPartType::RoadsideGutter:
		case RoadPartType::Gutter:
			return Max(0.120f, heightOffset + 0.035f);
		case RoadPartType::Sidewalk:
			return Max(0.030f, heightOffset + 0.008f);
		case RoadPartType::Shoulder:
		case RoadPartType::Median:
			return Max(0.025f, heightOffset + 0.006f);
		default:
			return 0.0f;
		}
	}

	MeshData buildRoadPartModelStrip(const CubicBezier& bezier, const World& world,
	                                 const RoadPartDef& def, const PartModelData& model,
	                                 float offsetA_L, float offsetA_R,
	                                 float offsetB_L, float offsetB_R,
	                                 float sStart, float sEnd, float lodFactor,
	                                 bool useElevation)
	{
		if (model.vertices.isEmpty() || model.indices.isEmpty()) return MeshData{};
		const float spanLen = sEnd - sStart;
		if (spanLen <= 0.1f) return MeshData{};
		const float totalLength = bezier.totalLength;
		const float unitLen = Max(0.1f, def.modelUnitLen);
		const float unitWidth = Max(0.001f, def.modelUnitWidth);
		const int segmentCount = Clamp(static_cast<int>(spanLen / unitLen * lodFactor) + 1,
			3, static_cast<int>(120 * lodFactor));

		MeshData md;
		md.vertices.reserve(model.vertices.size() * segmentCount);
		md.indices.reserve(model.indices.size() * segmentCount);

		for (int segment = 0; segment < segmentCount; ++segment)
		{
			const float segT0 = segment / static_cast<float>(segmentCount);
			const float segT1 = (segment + 1) / static_cast<float>(segmentCount);
			const float segS0 = Math::Lerp(sStart, sEnd, segT0);
			const float segS1 = Math::Lerp(sStart, sEnd, segT1);
			const uint32 indexBase = static_cast<uint32>(md.vertices.size());

			for (const Vertex3D& src : model.vertices)
			{
				const float localAcross = Clamp(src.pos.x / unitWidth, 0.0f, 1.0f);
				const float localAlong = Clamp(src.pos.z / unitLen, 0.0f, 1.0f);
				const float s = Math::Lerp(segS0, segS1, localAlong);
				const float roadT = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
				const float offsetL = Math::Lerp(offsetA_L, offsetB_L, roadT);
				const float offsetR = Math::Lerp(offsetA_R, offsetB_R, roadT);
				const float lateral = Math::Lerp(offsetL, offsetR, localAcross);
				const auto sl = makeSlice(bezier, world, s, kRoadSurfaceLift, useElevation);
				Vec3 tangent = bezier.tangentAt(s);
				if (tangent.lengthSq() <= 1e-8) tangent = Vec3{ 1.0, 0.0, 0.0 };
				tangent.normalize();

				Vertex3D dst = src;
				Vec3 pos = sl.center + sl.right * static_cast<double>(lateral) + Vec3{ 0.0, src.pos.y + def.heightOffset, 0.0 };
				if (!useElevation)
				{
					pos.y = sl.center.y + src.pos.y + def.heightOffset;
				}
				dst.pos = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
				dst.normal = Float3{
					static_cast<float>(sl.right.x) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.x) * src.normal.z,
					src.normal.y,
					static_cast<float>(sl.right.z) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.z) * src.normal.z
				};
				dst.tex = def.type == RoadPartType::Sidewalk || def.type == RoadPartType::Curb
					? Float2{lateral, s} : Float2{src.tex.x + static_cast<float>(segment), src.tex.y};
				md.vertices << dst;
			}

			for (const TriangleIndex32& tri : model.indices)
			{
				md.indices << TriangleIndex32{ tri.i0 + indexBase, tri.i1 + indexBase, tri.i2 + indexBase };
			}
		}

		return md;
	}

}

MeshData RoadRenderer::buildStripMesh(const CubicBezier& bezier, const World& world,
                                      float offsetL, float offsetR, float heightOffset,
                                      float sStart, float sEnd, float lodFactor,
                                      bool useElevation) const
{
	const float spanLen = sEnd - sStart;
	if (spanLen <= 0.1f) return MeshData{};

	const int N = Clamp(static_cast<int>(spanLen / 2.0f * lodFactor) + 1,
	                    3, static_cast<int>(100 * lodFactor));

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
		const Vec3  pL = sl.center + sl.right * static_cast<double>(offsetL);
		const Vec3  pR = sl.center + sl.right * static_cast<double>(offsetR);
		vertices << makeVertWorldUV(pL);
		vertices << makeVertWorldUV(pR);
	}

	Array<TriangleIndex32> indices;
	indices.reserve(N * 2);
	for (int i = 0; i < N; ++i)
	{
		appendQuad(indices,
		           static_cast<uint32>(i * 2),
		           static_cast<uint32>(i * 2 + 1),
		           static_cast<uint32>(i * 2 + 2),
		           static_cast<uint32>(i * 2 + 3));
	}

	return MeshData{ vertices, indices };
}

MeshData RoadRenderer::buildStripMeshTapered(const CubicBezier& bezier, const World& world,
                                              float offsetA_L, float offsetA_R,
                                              float offsetB_L, float offsetB_R,
                                              float heightOffset,
                                              float sStart, float sEnd, float lodFactor,
                                              float sideSkirtDrop,
                                              bool useElevation) const
{
	const float spanLen = sEnd - sStart;
	if (spanLen <= 0.1f) return MeshData{};

	const float totalLength = bezier.totalLength;

	const int N = Clamp(static_cast<int>(spanLen / 2.0f * lodFactor) + 1,
	                    3, static_cast<int>(100 * lodFactor));
	const bool addSideSkirts = (sideSkirtDrop > 0.001f);

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * (addSideSkirts ? 6 : 2));

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const float t  = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
		const float oL = Math::Lerp(offsetA_L, offsetB_L, t);
		const float oR = Math::Lerp(offsetA_R, offsetB_R, t);
		const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
		Vec3 pL = sl.center + sl.right * static_cast<double>(oL);
		Vec3 pR = sl.center + sl.right * static_cast<double>(oR);
		if (!useElevation)
		{
			pL.y = sl.center.y;
			pR.y = sl.center.y;
		}
		vertices << makeVertWorldUV(pL);
		vertices << makeVertWorldUV(pR);
	}

	Array<TriangleIndex32> indices;
	indices.reserve(N * (addSideSkirts ? 6 : 2));
	for (int i = 0; i < N; ++i)
	{
		appendQuad(indices,
		           static_cast<uint32>(i * 2),
		           static_cast<uint32>(i * 2 + 1),
		           static_cast<uint32>(i * 2 + 2),
		           static_cast<uint32>(i * 2 + 3));
	}

	if (addSideSkirts)
	{
		const uint32 sideBase = static_cast<uint32>(vertices.size());
		for (int i = 0; i <= N; ++i)
		{
			const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
			const float t  = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
			const float oL = Math::Lerp(offsetA_L, offsetB_L, t);
			const float oR = Math::Lerp(offsetA_R, offsetB_R, t);
			const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
			Vec3 pL = sl.center + sl.right * static_cast<double>(oL);
			Vec3 pR = sl.center + sl.right * static_cast<double>(oR);
			if (!useElevation)
			{
				pL.y = sl.center.y;
				pR.y = sl.center.y;
			}
			const Vec3  bL{ pL.x, pL.y - static_cast<double>(sideSkirtDrop), pL.z };
			const Vec3  bR{ pR.x, pR.y - static_cast<double>(sideSkirtDrop), pR.z };
			const Float3 leftNormal{ static_cast<float>(-sl.right.x), 0.0f, static_cast<float>(-sl.right.z) };
			const Float3 rightNormal{ static_cast<float>(sl.right.x), 0.0f, static_cast<float>(sl.right.z) };
			vertices << makeVertWorldUVWithNormal(pL, leftNormal);
			vertices << makeVertWorldUVWithNormal(bL, leftNormal);
			vertices << makeVertWorldUVWithNormal(pR, rightNormal);
			vertices << makeVertWorldUVWithNormal(bR, rightNormal);
		}
		for (int i = 0; i < N; ++i)
		{
			const uint32 a = sideBase + static_cast<uint32>(i * 4);
			const uint32 b = sideBase + static_cast<uint32>((i + 1) * 4);
			indices << TriangleIndex32{ a, b + 1, a + 1 };
			indices << TriangleIndex32{ a, b, b + 1 };
			indices << TriangleIndex32{ a + 2, a + 3, b + 3 };
			indices << TriangleIndex32{ a + 2, b + 3, b + 2 };
		}
	}

	return MeshData{ vertices, indices };
}
Array<PartMeshEntry> RoadRenderer::buildPartMeshes(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
                                                    const World& world,
                                                    float marginA, float marginB)
{
	// 断面部品をテーパー付きの帯メッシュへ展開し、近景と遠景の両 LOD を同時に用意する。
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	Array<PartMeshEntry> entries;
	struct MaterialGroup
	{
		const Texture* tex;
		ColorF         color;
		MeshData       md;
	};
	Array<MaterialGroup> groups;


	const MeshData overlappingPavement = adjacentPavement(network,edge,world);

	for (const auto& part : edge.parts)
	{
		if (part.build != BuildState::Built || part.placement != RoadPartPlacement::Strip) continue;

		auto [color, heightOff, tex] = getPartVisual(part);
		if (part.asphalt())
		{
			if (edge.roadType == RoadType::Arterial) color = ColorF{ 0.29, 0.30, 0.32 };
			else if (edge.roadType == RoadType::LocalRoad) color = ColorF{ 0.29, 0.30, 0.32 };
		}

		const RoadPartDef& def = m_partRegistry.get(part.defId);
		const RoadPartModel& model = m_partRegistry.getModel(part.defId);
		MeshData mdDetail;
		MeshData mdLod;
		if (part.type != RoadPartType::Roadbed && !model.center.isEmpty())
		{
			mdDetail = buildRoadPartModelStrip(bezier, world, def, model.center,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 1.0f, edge.usesDesignHeight());
			const PartModelData& lodModel = (!model.lods.isEmpty() && !model.lods[0].center.isEmpty()) ? model.lods[0].center : model.center;
			mdLod = buildRoadPartModelStrip(bezier, world, def, lodModel,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 0.25f, edge.usesDesignHeight());
		}
		else
		{
			mdDetail = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 1.0f, roadPartSideSkirtDrop(part.type, heightOff), edge.usesDesignHeight());
			mdLod = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 0.25f, roadPartSideSkirtDrop(part.type, heightOff), edge.usesDesignHeight());
		}
		if (part.type != RoadPartType::Roadbed && !overlappingPavement.indices.isEmpty())
		{
			mdDetail = RoadGeometry::excludeSurface(mdDetail,overlappingPavement);
			mdLod = RoadGeometry::excludeSurface(mdLod,overlappingPavement);
		}
		if (mdDetail.vertices.isEmpty()) continue;

		PartMeshEntry entry;
		orientRoadFaces(mdDetail);
		orientRoadFaces(mdLod);
		entry.meshPair.detail = Mesh{ mdDetail };
		entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
		entry.color   = color;
		entry.texture = tex;
		entry.materialType = part.type;
		entry.asphalt = part.asphalt();
		entries << std::move(entry);
		// 既存グループを検索（部品数は通常 ≤10 なので線形探索で十分）
		MaterialGroup* group = nullptr;
		for (auto& g : groups)
		{
			if (g.tex == tex && g.color == color)
			{
				group = &g;
				break;
			}
		}
		if (!group)
		{
			groups << MaterialGroup{ tex, color, MeshData{} };
			group = &groups.back();
		}

		const uint32 indexBase = static_cast<uint32>(group->md.vertices.size());
		group->md.vertices.append(mdLod.vertices);
		for (const auto& tri : mdLod.indices)
		{
			group->md.indices << TriangleIndex32{ tri.i0 + indexBase, tri.i1 + indexBase, tri.i2 + indexBase };
		}

	}

	auto& batches=m_partLodBatchCache[edge.id];
	batches.clear();
	batches.reserve(groups.size());
	for (auto& g : groups)
	{
		if (g.md.vertices.isEmpty()) continue;
		orientRoadFaces(g.md);
		batches << PartLodBatch{ Mesh{ g.md }, g.color, g.tex };
	}
	return entries;
}

Array<PartMeshEntry> RoadRenderer::buildNodeCapParts(const RoadNetwork& network, int nodeId,
                                                     const World& world, [[maybe_unused]] int div,
                                                     bool onlyOpenEdges)
{
	const auto layout = JunctionGeometry::build(network, nodeId, onlyOpenEdges, &world);
	Array<PartMeshEntry> entries;
	if (layout.asphalt.vertices.isEmpty()) { return entries; }
	auto addEntry = [&](MeshData mesh, ColorF color, const Texture* texture, RoadPartType type = RoadPartType::Roadbed)
	{
		if (mesh.vertices.isEmpty() || mesh.indices.isEmpty()) { return; }
		orientRoadFaces(mesh);
		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mesh };
		// Keep the same boundary at both distances so the terrain cut never opens a gap.
		entry.meshPair.lod = entry.meshPair.detail;
		entry.color = color;
		entry.texture = texture;
		entry.materialType = type;
		entries << std::move(entry);
	};
	MeshData asphalt = layout.asphalt;
	for (auto& vertex : asphalt.vertices)
	{
		if (!layout.elevated) { vertex.pos.y = static_cast<float>(world.sampleHeight(vertex.pos.x, vertex.pos.z) + kRoadSurfaceLift); }
	}
	const auto& roadbedDefinition = m_partRegistry.get(U"roadbed_asphalt");
	ColorF asphaltColor{ 0.29, 0.30, 0.32 };
	for (const auto& attachment : network.getNode(nodeId)->attachments)
	{
		const auto* edge = network.getEdge(attachment.edgeId);
		if (edge && edge->roadType == RoadType::Arterial) { asphaltColor = ColorF{ 0.29, 0.30, 0.32 }; break; }
	}
	addEntry(std::move(asphalt), asphaltColor, roadbedDefinition.texture ? &*roadbedDefinition.texture : nullptr);
	// Acute arms may overlap beyond their nominal cut mouths. Clip roadside
	// bands against the entire connected pavement, not only the cap polygon.
	MeshData connectedPavement = layout.asphalt;
	for (const auto& attachment : network.getNode(nodeId)->attachments)
	{
		const auto* edge = network.getEdge(attachment.edgeId);
		if (!edge || !edge->isRoadbedBuilt() || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing)) { continue; }
		if (const auto bezier = network.getBezier(edge->id)) { appendMeshData(connectedPavement,RoadGeometry::roadbedSurface(*edge,*bezier,world)); }
	}
	for (const auto& corner : layout.corners)
	{
		for (const auto& band : corner.bands)
		{
			const auto [color, heightOffset, texture] = getPartVisual(band.part);
			const auto& definition = m_partRegistry.get(band.part.defId);
			const auto& model = m_partRegistry.getModel(band.part.defId).center;
			MeshData mesh;
			for (size_t segment = 1; segment < corner.sections.size(); ++segment)
			{
				const auto& start = corner.sections[segment-1];
				const auto& end = corner.sections[segment];
				const uint32 base = static_cast<uint32>(mesh.vertices.size());
				if (!model.isEmpty())
				{
					for (const auto& source : model.vertices)
					{
						const double across = Clamp(source.pos.x / Max(0.001f, definition.modelUnitWidth), 0.0f, 1.0f);
						const double along = Clamp(source.pos.z / Max(0.1f, definition.modelUnitLen), 0.0f, 1.0f);
						Vec3 position = JunctionGeometry::bandPosition(start, band, across).lerp(JunctionGeometry::bandPosition(end, band, across), along);
						if (!layout.elevated) { position.y = world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.z)) + kRoadSurfaceLift; }
						position.y += source.pos.y + heightOffset;
						Vertex3D vertex = makeVertWorldUV(position);
						const Vec3 outward = start.outward.lerp(end.outward, along).normalized();
						const Vec3 alongVector{ -outward.z, 0.0, outward.x };
						vertex.normal = Float3{ outward * source.normal.x + Vec3{ 0.0, source.normal.y, 0.0 } + alongVector * source.normal.z };
						mesh.vertices << vertex;
					}
					for (const auto& triangle : model.indices)
					{
						mesh.indices << TriangleIndex32{ base+triangle.i0, base+triangle.i1, base+triangle.i2 };
					}
				}
				else
				{
					for (const auto& section : { start, end })
					{
						for (const double across : { 0.0, 1.0 })
						{
							Vec3 position = JunctionGeometry::bandPosition(section, band, across);
							if (!layout.elevated) { position.y = world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.z)) + kRoadSurfaceLift; }
							position.y += heightOffset;
							mesh.vertices << makeVertWorldUV(position);
						}
					}
					appendQuad(mesh.indices, base, base+1, base+2, base+3);
				}
			}
			mesh = RoadGeometry::excludeSurface(mesh,connectedPavement);
			addEntry(std::move(mesh), color, texture, band.part.type);
		}
	}
	return entries;
}
