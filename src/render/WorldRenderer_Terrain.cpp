#include "WorldRenderer.hpp"
#include "TerrainSurfaceGeometry.hpp"
#include "TerrainMaterials.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/JunctionGeometry.hpp"

using namespace TerrainSurfaceGeometry;

namespace
{
	constexpr float kRoadTerrainQuadStep  = 24.0f;
	constexpr float kRoadTerrainRelief    = 0.012f;
	constexpr float kChunkSizeF           = static_cast<float>(CHUNK_SIZE);
	float roadEdgeOffsetAt(const RoadPart& part, float ft, bool leftSide)
	{
		return leftSide
			? Math::Lerp(part.offsetA_L, part.offsetB_L, ft)
			: Math::Lerp(part.offsetA_R, part.offsetB_R, ft);
	}

	bool calcRoadCrossSectionRange(const RoadEdge& edge, float ft, float& outLeft, float& outRight)
	{
		bool found = false;
		for (const auto& part : edge.parts)
		{
			if (!RoadGeometry::isStructuralStrip(part))
			{
				continue;
			}

			const float left  = roadEdgeOffsetAt(part, ft, true);
			const float right = roadEdgeOffsetAt(part, ft, false);
			if (!found)
			{
				outLeft = Min(left, right);
				outRight = Max(left, right);
				found = true;
			}
			else
			{
				outLeft = Min(outLeft, Min(left, right));
				outRight = Max(outRight, Max(left, right));
			}
		}
		return found;
	}

	int terrainMaterialKeyForCell(const Chunk& chunk, int col, int row)
	{
		const int x = Clamp(col, 0, ZONE_CELLS - 1);
		const int z = Clamp(row, 0, ZONE_CELLS - 1);
		const float h = (chunk.heightMap[{ x, z }] + chunk.heightMap[{ x + 1, z }]
			+ chunk.heightMap[{ x, z + 1 }] + chunk.heightMap[{ x + 1, z + 1 }]) * 0.25f;
		// 建て込んだ用途地域の建物間は芝生でなく空き地の地面にする。
		const ZoneType zone = chunk.zoneMap[{ x, z }];
		if (zone == ZoneType::Residential || zone == ZoneType::LowResidential
			|| zone == ZoneType::Commercial || zone == ZoneType::Industrial)
		{
			int built = 0;
			for (int dz = -1; dz <= 1; ++dz)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = x + dx, nz = z + dz;
					if (nx < 0 || nz < 0 || nx >= ZONE_CELLS || nz >= ZONE_CELLS) { continue; }
					const BuildingType type = chunk.buildingGrid[{ nx, nz }].type;
					built += type != BuildingType::None && type != BuildingType::Farmland && type != BuildingType::ParkBuilding;
				}
			}
			if (built >= 2) { return TerrainMaterials::kUrbanGround; }
		}
		return TerrainMaterials::keyForHeight(h);
	}

	std::unordered_set<int64> chunkKeysAroundPoint(const Vec3& pos, float radius)
	{
		std::unordered_set<int64> keys;
		const float minX = static_cast<float>(pos.x) - radius;
		const float maxX = static_cast<float>(pos.x) + radius;
		const float minZ = static_cast<float>(pos.z) - radius;
		const float maxZ = static_cast<float>(pos.z) + radius;
		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(minX / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor(maxX / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(minZ / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor(maxZ / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				keys.insert(chunkCoordToKey(Point{ cx, cz }));
		return keys;
	}

	String formatChunkKeySet(const std::unordered_set<int64>& keys)
	{
		Array<String> parts;
		parts.reserve(keys.size());
		for (const int64 key : keys)
		{
			const Point chunk{
				static_cast<int>(key >> 32),
				static_cast<int>(static_cast<uint32>(key))
			};
			parts << U"({}, {})"_fmt(chunk.x, chunk.y);
		}
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

	String formatChunkKeyArray(const Array<int64>& keys)
	{
		Array<String> parts;
		parts.reserve(keys.size());
		for (const int64 key : keys)
		{
			const Point chunk{
				static_cast<int>(key >> 32),
				static_cast<int>(static_cast<uint32>(key))
			};
			parts << U"({}, {})"_fmt(chunk.x, chunk.y);
		}
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

	String formatIntArray(const Array<int>& values)
	{
		Array<String> parts;
		parts.reserve(values.size());
		for (const int value : values)
			parts << ToString(value);
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

}

Array<WorldRenderer::TerrainMeshData> WorldRenderer::buildTerrainMeshData(const Chunk& chunk, const Array<TerrainSubtractionQuad>& quads)
{
	const Vec3 worldOrigin = chunk.worldOrigin();
	const Stopwatch buildTimer{ StartImmediately::Yes };
	// Bucket road footprints per terrain cell. Dense towns keep the same geometry as villages.
	Grid<Array<size_t>> cellQuads(HEIGHT_CELLS, HEIGHT_CELLS);
	size_t candidateTests = 0;
	for (size_t index = 0; index < quads.size(); ++index)
	{
		const RectF& bounds = quads[index].bounds;
		const int minCol = Clamp(static_cast<int>(Floor((bounds.x - worldOrigin.x) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int maxCol = Clamp(static_cast<int>(Floor((bounds.x + bounds.w - worldOrigin.x) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int minRow = Clamp(static_cast<int>(Floor((bounds.y - worldOrigin.z) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int maxRow = Clamp(static_cast<int>(Floor((bounds.y + bounds.h - worldOrigin.z) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		for (int row = minRow; row <= maxRow; ++row)
		{
			for (int col = minCol; col <= maxCol; ++col)
			{
				cellQuads[{ col, row }] << index;
			}
		}
	}

	HashTable<int, MeshData> groups;

	auto makeGridVertex = [&](int col, int row)
	{
		const Vec3 pos = worldOrigin + Vec3{
			col * kTerrainCellSize,
			chunk.heightMap[{ col, row }],
			row * kTerrainCellSize
		};
		return TerrainClipVertex{ pos, terrainUvAt(pos) };
	};

	auto processTriangle = [&](int materialKey, const Array<size_t>& candidates, const TerrainClipVertex& a, const TerrainClipVertex& b, const TerrainClipVertex& c)
	{
		Array<Array<TerrainClipVertex>> pieces;
		pieces << Array<TerrainClipVertex>{ a, b, c };

		if (!candidates.isEmpty())
		{
			RectF triBounds = boundsOfPolygon({
				Vec2{ static_cast<float>(a.pos.x), static_cast<float>(a.pos.z) },
				Vec2{ static_cast<float>(b.pos.x), static_cast<float>(b.pos.z) },
				Vec2{ static_cast<float>(c.pos.x), static_cast<float>(c.pos.z) },
			});

			for (const size_t index : candidates)
			{
				++candidateTests;
				const auto& quad = quads[index];
				if (!rectIntersects(triBounds, quad.bounds)) continue;
				if (!quad.cutPlanes.isEmpty())
				{
					Array<Array<TerrainClipVertex>> next;
					for (const auto& piece : pieces)
					{
						MeshBoolean::Face face;
						for (const auto& vertex : piece) { face<<Vertex3D{Float3{vertex.pos},Float3{0,1,0},vertex.tex}; }
						for (const auto& fragment : MeshBoolean::subtractFace(face,quad.cutPlanes))
						{
							Array<TerrainClipVertex> polygon;
							for (const auto& vertex : fragment) { polygon<<TerrainClipVertex{Vec3{vertex.pos},vertex.tex}; }
							next<<std::move(polygon);
						}
					}
					pieces=std::move(next);continue;
				}
				// The replacement soil must also remain below the actual sampled terrain,
				// since node Bezier heights and the rendered terrain can differ.
				const float floorY = Min(quad.bedBottomY,
					static_cast<float>(Min(Min(a.pos.y, b.pos.y), c.pos.y)) - 0.04f);

				Array<Array<TerrainClipVertex>> nextPieces;
				for (const auto& piece : pieces)
				{
					Array<TerrainClipVertex> below = clipPolygonByHeight(piece, floorY, true);
					if (below.size() >= 3) nextPieces << std::move(below);

					Array<TerrainClipVertex> above = clipPolygonByHeight(piece, floorY, false);
					if (above.size() >= 3)
					{
						Array<Array<TerrainClipVertex>> kept = subtractConvexPolygonXZ(above, quad.footprint, floorY);
						for (auto& poly : kept)
						{
							if (poly.size() >= 3) nextPieces << std::move(poly);
						}
					}
				}

				pieces = std::move(nextPieces);
				if (pieces.isEmpty()) break;
			}
		}

		MeshData& group = groups[materialKey];
		for (const auto& poly : pieces)
		{
			Array<TerrainClipVertex> materialPoly;
			materialPoly.reserve(poly.size());
			for (const TerrainClipVertex& v : poly) materialPoly << withMaterialUv(v, materialKey);
			appendPolygonAsTriangles(materialPoly, group.vertices, group.indices);
		}
	};

	for (int row = 0; row < HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col < HEIGHT_CELLS; ++col)
		{
			const TerrainClipVertex v00 = makeGridVertex(col, row);
			const TerrainClipVertex v10 = makeGridVertex(col + 1, row);
			const TerrainClipVertex v01 = makeGridVertex(col, row + 1);
			const TerrainClipVertex v11 = makeGridVertex(col + 1, row + 1);

			const int materialKey = terrainMaterialKeyForCell(chunk, col, row);
			processTriangle(materialKey, cellQuads[{ col, row }], v00, v01, v10);
			processTriangle(materialKey, cellQuads[{ col, row }], v10, v01, v11);
		}
	}

	(void)candidateTests;
	(void)buildTimer;
	Array<TerrainMeshData> result;
	result.reserve(groups.size());
	for (auto& [materialKey, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		result << TerrainMeshData{ materialKey, std::move(meshData) };
	}
	return result;
}

const WorldRenderer::TerrainBooleanSubtractor* WorldRenderer::getTerrainSubtractor(
	const RoadNetwork& network, int edgeId)
{
	if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		return &it->second;

	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing && edge->edgeState != EdgeState::UnderConstruction) || edge->parts.isEmpty())
		return nullptr;

	const auto bezOpt = network.getBezier(edgeId);
	if (!bezOpt || bezOpt->totalLength <= 0.0f)
		return nullptr;

	TerrainBooleanSubtractor subtractor;
	subtractor.edgeId = edgeId;
	std::unordered_set<Key> touchedChunkSet;
	auto registerSubtractionFootprint = [&](Array<Vec2> footprint, float bedBottomY)
	{
		if (footprint.size() < 3) return;
		if (signedAreaXZ(footprint) < 0.0f)
			footprint.reverse();

		const RectF bounds = boundsOfPolygon(footprint);
		if (bounds.w <= kTerrainClipEpsilon || bounds.h <= kTerrainClipEpsilon)
			return;

		TerrainSubtractionQuad quad;
		quad.footprint = std::move(footprint);
		quad.bounds = bounds;
		quad.bedBottomY = bedBottomY;
		subtractor.quads << quad;

		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(bounds.x / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor((bounds.x + bounds.w) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(bounds.y / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor((bounds.y + bounds.h) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				touchedChunkSet.insert(chunkCoordToKey(Point{ cx, cz }));
	};

	if(edge->useElevation || edge->tunnel)
	{
		for(const auto& object:network.objects())
		{
			if(object.id<0 || object.parentEdgeId!=edgeId || object.type!=RoadObjectType::Pier) continue;
			const Vec3 position=bezOpt->positionAt(object.arcPos);
			const Vec3 right=tangentToRight(bezOpt->tangentAt(object.arcPos)),along{-right.z,0,right.x};
			const double halfWidth=edge->edgeState==EdgeState::UnderConstruction ? 3.0 : 2.4;
			Array<Vec2> footprint;
			for(const Vec2 corner:{Vec2{-1,-1},Vec2{1,-1},Vec2{1,1},Vec2{-1,1}})
			{
				const Vec3 p=position+right*corner.x*halfWidth+along*corner.y*halfWidth;
				footprint << Vec2{p.x,p.z};
			}
			// Shallow foundation bed; the simulation heightfield remains unchanged.
			registerSubtractionFootprint(std::move(footprint),static_cast<float>(position.y)+100.0f);
		}
		if(subtractor.quads.isEmpty()) return nullptr;
		for(const Key key:touchedChunkSet) subtractor.touchedChunkKeys << key;
		return &m_edgeSubtractorCache.emplace(edgeId,std::move(subtractor)).first->second;
	}

	const CubicBezier& bez = *bezOpt;
	const float startArc = edge->edgeState == EdgeState::UnderConstruction ? 0.0f : Max(0.0f, edge->cutoffA - 0.1f);
	const float endArc = edge->edgeState == EdgeState::UnderConstruction ? bez.totalLength : Min(bez.totalLength, bez.totalLength - edge->cutoffB + 0.1f);
	const float visibleLength = Max(0.0f, endArc - startArc);
	const int sampleCount = Max(2, static_cast<int>(Math::Ceil(visibleLength / kRoadTerrainQuadStep)) + 1);

	struct EdgeSample
	{
		Vec3  center;
		Vec2  left;
		Vec2  right;
		float bedBottomY = 0.0f;
	};

	Array<EdgeSample> samples;
	samples.reserve(sampleCount);

	for (int i = 0; i < sampleCount; ++i)
	{
		const float ft = (sampleCount <= 1) ? 0.0f : (i / static_cast<float>(sampleCount - 1));
		const float s = Math::Lerp(startArc, endArc, ft);
		const Vec3 center = bez.positionAt(s);
		float leftOffset = 0.0f;
		float rightOffset = 0.0f;
		if (!calcRoadCrossSectionRange(*edge, s / bez.totalLength, leftOffset, rightOffset))
			continue;

		const Vec3 rightVec3 = tangentToRight(bez.tangentAt(s));
		const Vec2 rightVec{ static_cast<float>(rightVec3.x), static_cast<float>(rightVec3.z) };
		EdgeSample sample;
		sample.center = center;
		sample.left = Vec2{
			static_cast<float>(center.x + rightVec.x * leftOffset),
			static_cast<float>(center.z + rightVec.y * leftOffset)
		};
		sample.right = Vec2{
			static_cast<float>(center.x + rightVec.x * rightOffset),
			static_cast<float>(center.z + rightVec.y * rightOffset)
		};
		sample.bedBottomY = static_cast<float>(center.y + kRoadSurfaceLift - (edge->edgeState==EdgeState::UnderConstruction ? .25 : kRoadTerrainRelief));
		samples << sample;
	}

	if (samples.size() < 2)
		return nullptr;

	for (size_t i = 0; i + 1 < samples.size(); ++i)
	{
		Array<Vec2> footprint = {
			samples[i].left,
			samples[i + 1].left,
			samples[i + 1].right,
			samples[i].right,
		};
		registerSubtractionFootprint(std::move(footprint),
			Min(samples[i].bedBottomY, samples[i + 1].bedBottomY));
	}

	if (subtractor.quads.isEmpty())
		return nullptr;

	for (const Key key : touchedChunkSet)
		subtractor.touchedChunkKeys << key;
	auto [it, inserted] = m_edgeSubtractorCache.emplace(edgeId, std::move(subtractor));
	return &it->second;
}

const WorldRenderer::TerrainNodeSubtractor* WorldRenderer::getTerrainNodeSubtractor(
	const RoadNetwork& network, int nodeId)
{
	if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
		return &it->second;

	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2)
		return nullptr;

	if (node->type != NodeType::Intersection && node->type != NodeType::Diverge && node->type != NodeType::Joint)
		return nullptr;

	bool underground=true;
	for (const auto& attachment : node->attachments) { const auto* edge=network.getEdge(attachment.edgeId); underground &= edge && edge->tunnel; }
	if (underground) { return nullptr; }
	TerrainNodeSubtractor subtractor;
	subtractor.nodeId = nodeId;
	std::unordered_set<Key> touchedChunkSet;
	auto registerSubtractionFootprint = [&](Array<Vec2> footprint, float bedBottomY)
	{
		if (footprint.size() < 3) return;
		if (signedAreaXZ(footprint) < 0.0f)
			footprint.reverse();

		const RectF bounds = boundsOfPolygon(footprint);
		if (bounds.w <= kTerrainClipEpsilon || bounds.h <= kTerrainClipEpsilon)
			return;

		TerrainSubtractionQuad quad;
		quad.footprint = std::move(footprint);
		quad.bounds = bounds;
		quad.bedBottomY = bedBottomY;
		subtractor.quads << quad;

		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(bounds.x / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor((bounds.x + bounds.w) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(bounds.y / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor((bounds.y + bounds.h) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				touchedChunkSet.insert(chunkCoordToKey(Point{ cx, cz }));
	};

	const auto layout = JunctionGeometry::build(network, nodeId);
	if (layout.elevated && !layout.groundConnected) { return nullptr; }
	const MeshData footprint = JunctionGeometry::terrainFootprint(layout);
	// Merge only adjacent triangles whose union remains convex. This preserves the
	// concave kerb exactly while avoiding hundreds of repeated cuts in one terrain cell.
	Array<Array<uint32>> pieces;
	for (const auto& triangle : footprint.indices) { pieces << Array<uint32>{ triangle.i0, triangle.i1, triangle.i2 }; }
	for (size_t i = 0; i < pieces.size(); ++i)
	{
		bool merged = true;
		while (merged)
		{
			merged = false;
			for (size_t j = i + 1; j < pieces.size() && !merged; ++j)
			{
				const auto& a = pieces[i];
				const auto& b = pieces[j];
				if (a.size() + b.size() > 18) { continue; }
				for (size_t ia = 0; ia < a.size() && !merged; ++ia)
				{
					for (size_t ib = 0; ib < b.size() && !merged; ++ib)
					{
						if (a[ia] != b[(ib+1)%b.size()] || a[(ia+1)%a.size()] != b[ib]) { continue; }
						Array<uint32> joined;
						for (size_t offset = 1; offset <= a.size(); ++offset) { joined << a[(ia+offset)%a.size()]; }
						for (size_t offset = 2; offset < b.size(); ++offset) { joined << b[(ib+offset)%b.size()]; }
						double sign = 0.0;
						bool convex = true;
						for (size_t k = 0; k < joined.size(); ++k)
						{
							const auto p = footprint.vertices[joined[k]].pos;
							const auto q = footprint.vertices[joined[(k+1)%joined.size()]].pos;
							const auto r = footprint.vertices[joined[(k+2)%joined.size()]].pos;
							const double turn = (q.x-p.x)*(r.z-q.z) - (q.z-p.z)*(r.x-q.x);
							if (Abs(turn) < 1e-6) { continue; }
							if (sign * turn < 0.0) { convex = false; break; }
							sign = turn;
						}
						if (!convex) { continue; }
						pieces[i] = std::move(joined);
						pieces.erase(pieces.begin() + j);
						merged = true;
					}
				}
			}
		}
	}
	for (const auto& piece : pieces)
	{
		Array<Vec2> polygon;
		float floor = Math::InfF;
		for (const uint32 index : piece)
		{
			const auto& position = footprint.vertices[index].pos;
			polygon << Vec2{ position.x, position.z };
			floor = Min(floor, position.y);
		}
		registerSubtractionFootprint(std::move(polygon), floor - kRoadTerrainRelief);
	}

	if (subtractor.quads.isEmpty())
		return nullptr;

	for (const Key key : touchedChunkSet)
		subtractor.touchedChunkKeys << key;
	auto [it, inserted] = m_nodeSubtractorCache.emplace(nodeId, std::move(subtractor));
	return &it->second;
}

const Array<WorldRenderer::TerrainSubtractionQuad>& WorldRenderer::getChunkSubtractionQuads(
	const RoadNetwork& network, Point chunkCoord)
{
	const Key chunkKey = chunkCoordToKey(chunkCoord);
	if (const auto it = m_chunkSubtractorCache.find(chunkKey); it != m_chunkSubtractorCache.end())
		return it->second;

	const RectF chunkBounds{
		static_cast<float>(chunkCoord.x * CHUNK_SIZE),
		static_cast<float>(chunkCoord.y * CHUNK_SIZE),
		static_cast<float>(CHUNK_SIZE),
		static_cast<float>(CHUNK_SIZE)
	};

	if (!m_chunkSubtractorPrimed)
	{
		primeAllChunkSubtractorCaches(network);
		if (const auto it = m_chunkSubtractorCache.find(chunkKey); it != m_chunkSubtractorCache.end())
			return it->second;
	}

	Array<TerrainSubtractionQuad> assembled;
	for (const auto& opening : m_tunnelOpenings) { if (opening.bounds.intersects(chunkBounds)) { assembled << TerrainSubtractionQuad{opening.footprint,opening.bounds,opening.floor,opening.planes}; } }
	Array<int> edgeContributors;
	for (const auto& entry : m_edgeSubtractorCache)
	{
		const TerrainBooleanSubtractor& subtractor = entry.second;
		bool touchesChunk = false;
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (key == chunkKey)
			{
				touchesChunk = true;
				break;
			}
		}
		if (!touchesChunk) continue;

		for (const auto& quad : subtractor.quads)
		{
			if (rectIntersects(chunkBounds, quad.bounds))
			{
				assembled << quad;
				if (!edgeContributors.contains(entry.first))
					edgeContributors << entry.first;
			}
		}
	}

	Array<int> nodeContributors;
	for (const auto& entry : m_nodeSubtractorCache)
	{
		const TerrainNodeSubtractor& subtractor = entry.second;
		bool touchesChunk = false;
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (key == chunkKey)
			{
				touchesChunk = true;
				break;
			}
		}
		if (!touchesChunk) continue;

		for (const auto& quad : subtractor.quads)
		{
			if (rectIntersects(chunkBounds, quad.bounds))
			{
				assembled << quad;
				if (!nodeContributors.contains(entry.first))
					nodeContributors << entry.first;
			}
		}
	}

	if (!assembled.isEmpty())
	{
		DBG_LOG(U"[TerrainBool] chunkSources chunk=({}, {}) quads={} edges={} nodes={}"_fmt(
			chunkCoord.x, chunkCoord.y, assembled.size(),
			formatIntArray(edgeContributors), formatIntArray(nodeContributors)));
	}

	auto [it, inserted] = m_chunkSubtractorCache.emplace(chunkKey, std::move(assembled));
	return it->second;
}

void WorldRenderer::primeAllChunkSubtractorCaches(const RoadNetwork& network)
{
	m_chunkSubtractorCache.clear();

	for (const auto& edge : network.edges())
	{
		if (edge.id < 0) continue;
		(void)getTerrainSubtractor(network, edge.id);
	}
	for (const auto& node : network.nodes())
	{
		if (node.id < 0) continue;
		(void)getTerrainNodeSubtractor(network, node.id);
	}

	for (const auto& [edgeId, subtractor] : m_edgeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			auto& bucket = m_chunkSubtractorCache[key];
			for (const auto& quad : subtractor.quads)
				bucket << quad;
		}
	}
	for (const auto& [nodeId, subtractor] : m_nodeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			auto& bucket = m_chunkSubtractorCache[key];
			for (const auto& quad : subtractor.quads)
				bucket << quad;
		}
	}

	for (const auto& opening : m_tunnelOpenings)
	{
		for (int z=Max(0,static_cast<int>(opening.bounds.y/CHUNK_SIZE));z<=Min(WORLD_CHUNKS-1,static_cast<int>(opening.bounds.br().y/CHUNK_SIZE));++z)
			for (int x=Max(0,static_cast<int>(opening.bounds.x/CHUNK_SIZE));x<=Min(WORLD_CHUNKS-1,static_cast<int>(opening.bounds.br().x/CHUNK_SIZE));++x)
				m_chunkSubtractorCache[chunkCoordToKey({x,z})] << TerrainSubtractionQuad{opening.footprint,opening.bounds,opening.floor,opening.planes};
	}
	m_chunkSubtractorPrimed = true;
}

void WorldRenderer::invalidateTerrainChunkKeys(const std::unordered_set<Key>& chunkKeys, bool rebuildImmediately)
{
	m_regionalTerrain.clear();
	m_transportSitesDirty=true;
	if (chunkKeys.empty()) return;

	for (const Key key : chunkKeys)
	{
		++m_terrainRevisions[key];
		m_chunkSubtractorCache.erase(key);
		if (rebuildImmediately)
		{
			m_meshCache.erase(key);
			m_pendingTerrainRebuildKeys.erase(key);
		}
		else
		{
			m_pendingTerrainRebuildKeys.insert(key);
		}
	}
}

void WorldRenderer::invalidateTerrainForEdges(const RoadNetwork& network, const Array<int>& edgeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> uniqueEdgeIds;
	for (const int edgeId : edgeIds)
	{
		if (edgeId >= 0)
			uniqueEdgeIds.insert(edgeId);
	}

	for (const int edgeId : uniqueEdgeIds)
	{
		if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_edgeSubtractorCache.erase(it);
		}

		if (const TerrainBooleanSubtractor* updated = getTerrainSubtractor(network, edgeId))
		{
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
		}
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys, true);
}

void WorldRenderer::invalidateTerrainNearDirtyNodes(const RoadNetwork& network, const Array<int>& nodeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> dirtyEdgeIds;
	std::unordered_set<int> uniqueNodeIds;

	for (const int nodeId : nodeIds)
	{
		if (nodeId < 0 || uniqueNodeIds.contains(nodeId)) continue;
		uniqueNodeIds.insert(nodeId);

		const RoadNode* node = network.getNode(nodeId);
		if (!node) continue;

		const auto nodeKeys = chunkKeysAroundPoint(node->position, 96.0f);
		dirtyChunkKeys.insert(nodeKeys.begin(), nodeKeys.end());

		for (const auto& att : node->attachments)
		{
			dirtyEdgeIds.insert(att.edgeId);
			const RoadEdge* edge = network.getEdge(att.edgeId);
			if (!edge) continue;

			const RoadNode* nodeA = network.getNode(edge->nodeA);
			const RoadNode* nodeB = network.getNode(edge->nodeB);
			if (nodeA)
			{
				const auto keys = chunkKeysAroundPoint(nodeA->position, 96.0f);
				dirtyChunkKeys.insert(keys.begin(), keys.end());
			}
			if (nodeB)
			{
				const auto keys = chunkKeysAroundPoint(nodeB->position, 96.0f);
				dirtyChunkKeys.insert(keys.begin(), keys.end());
			}
		}
	}

	for (const int edgeId : dirtyEdgeIds)
		m_edgeSubtractorCache.erase(edgeId);
	for (const int nodeId : uniqueNodeIds)
		m_nodeSubtractorCache.erase(nodeId);

	for (const int edgeId : dirtyEdgeIds)
		(void)getTerrainSubtractor(network, edgeId);
	for (const int nodeId : uniqueNodeIds)
		(void)getTerrainNodeSubtractor(network, nodeId);

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainNearMovedNode(const RoadNetwork& network, int nodeId, Vec3 oldNodePos)
{
	constexpr float InvalidationRadius = 96.0f;

	const RoadNode* node = network.getNode(nodeId);
	if (!node)
	{
		DBG_LOG(U"[TerrainBool] invalidateMoved node={} missing"_fmt(nodeId));
		invalidateTerrainForNode(nodeId);
		return;
	}

	DBG_LOG(U"[TerrainBool] invalidateMoved node={} old=({:.1f}, {:.1f}, {:.1f}) new=({:.1f}, {:.1f}, {:.1f})"_fmt(
		nodeId, oldNodePos.x, oldNodePos.y, oldNodePos.z, node->position.x, node->position.y, node->position.z));

	std::unordered_set<Key> dirtyChunkKeys;
	const auto oldNodeKeys = chunkKeysAroundPoint(oldNodePos, InvalidationRadius);
	dirtyChunkKeys.insert(oldNodeKeys.begin(), oldNodeKeys.end());

	const auto currentNodeKeys = chunkKeysAroundPoint(node->position, InvalidationRadius);
	dirtyChunkKeys.insert(currentNodeKeys.begin(), currentNodeKeys.end());

	std::unordered_set<int> dirtyEdgeIds;
	std::unordered_set<int> dirtyNodeIds;
	dirtyNodeIds.insert(nodeId);
	for (const auto& att : node->attachments)
	{
		dirtyEdgeIds.insert(att.edgeId);

		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) continue;
		if (edge->nodeA >= 0) dirtyNodeIds.insert(edge->nodeA);
		if (edge->nodeB >= 0) dirtyNodeIds.insert(edge->nodeB);

		if (const RoadNode* nodeA = network.getNode(edge->nodeA))
		{
			const auto keys = chunkKeysAroundPoint(nodeA->position, InvalidationRadius);
			dirtyChunkKeys.insert(keys.begin(), keys.end());
		}
		if (const RoadNode* nodeB = network.getNode(edge->nodeB))
		{
			const auto keys = chunkKeysAroundPoint(nodeB->position, InvalidationRadius);
			dirtyChunkKeys.insert(keys.begin(), keys.end());
		}
	}

	Array<int> expandedEdgeIds;
	for (const int edgeId : dirtyEdgeIds)
		expandedEdgeIds << edgeId;
	Array<int> expandedNodeIds;
	for (const int dirtyNodeId : dirtyNodeIds)
		expandedNodeIds << dirtyNodeId;
	DBG_LOG(U"[TerrainBool] invalidateMoved expandedEdges={} expandedNodes={}"_fmt(
		formatIntArray(expandedEdgeIds), formatIntArray(expandedNodeIds)));

	for (const int edgeId : dirtyEdgeIds)
	{
		if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_edgeSubtractorCache.erase(it);
		}
	}
	for (const int dirtyNodeId : dirtyNodeIds)
	{
		if (const auto it = m_nodeSubtractorCache.find(dirtyNodeId); it != m_nodeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_nodeSubtractorCache.erase(it);
		}
	}

	DBG_LOG(U"[TerrainBool] invalidateMoved node={} finalDirtyChunks={}"_fmt(
		nodeId, dirtyChunkKeys.size()));
	DBG_LOG(U"[TerrainBool] invalidateMoved finalChunks={}"_fmt(formatChunkKeySet(dirtyChunkKeys)));

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForNodes(const RoadNetwork& network, const Array<int>& nodeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> uniqueNodeIds;
	for (const int nodeId : nodeIds)
	{
		if (nodeId >= 0)
			uniqueNodeIds.insert(nodeId);
	}

	for (const int nodeId : uniqueNodeIds)
	{
		if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_nodeSubtractorCache.erase(it);
		}

		if (const TerrainNodeSubtractor* updated = getTerrainNodeSubtractor(network, nodeId))
		{
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
		}
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForEdge(int edgeId)
{
	if (edgeId < 0) return;

	std::unordered_set<Key> dirtyChunkKeys;
	if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
	{
		for (const Key key : it->second.touchedChunkKeys)
			dirtyChunkKeys.insert(key);
		m_edgeSubtractorCache.erase(it);
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForNode(int nodeId)
{
	if (nodeId < 0) return;

	std::unordered_set<Key> dirtyChunkKeys;
	if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
	{
		for (const Key key : it->second.touchedChunkKeys)
			dirtyChunkKeys.insert(key);
		m_nodeSubtractorCache.erase(it);
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateAllTerrain()
{
	m_regionalTerrain.clear();
	m_transportSitesDirty=true;
	++m_terrainEpoch;
	m_treeCache.clear();
	// Lot trees have the same lifetime as m_buildingMeshCache; preserve both.
	m_treeHeightRanges.clear();
	m_meshCache.clear();
	m_chunkSubtractorCache.clear();
	m_edgeSubtractorCache.clear();
	m_nodeSubtractorCache.clear();
	m_pendingTerrainRebuildKeys.clear();
	m_chunkSubtractorPrimed = false;
}

