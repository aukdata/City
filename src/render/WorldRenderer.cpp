#include "WorldRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include <Siv3D/Profiler.hpp>
#include <Siv3D/ViewFrustum.hpp>
#include <algorithm>

namespace
{
	constexpr float kTerrainCellSize      = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	constexpr float kRoadTerrainQuadStep  = 4.0f;
	constexpr float kRoadBedThickness     = 0.5f;
	constexpr float kTerrainClipEpsilon   = 1e-4f;
	constexpr float kChunkSizeF           = static_cast<float>(CHUNK_SIZE);
	constexpr float kTerrainTileSize      = static_cast<float>(CHUNK_SIZE) / (16.0f * 5.0f);
	constexpr float kTerrainUvCosA        = 0.97237f;
	constexpr float kTerrainUvSinA        = 0.23345f;

	struct TerrainClipVertex
	{
		Vec3   pos;
		Float2 tex;
	};

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

	bool calcRoadbedRangeAtNode(const RoadEdge& edge, bool isNodeA, float& outLeft, float& outRight)
	{
		bool found = false;
		for (const auto& part : edge.parts)
		{
			if (part.type != RoadPartType::Roadbed || part.build != BuildState::Built)
				continue;

			const float left = isNodeA ? part.offsetA_L : -part.offsetB_R;
			const float right = isNodeA ? part.offsetA_R : -part.offsetB_L;
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

	Float2 terrainUvAt(const Vec3& pos)
	{
		const float u = static_cast<float>(pos.x) / kTerrainTileSize;
		const float v = static_cast<float>(pos.z) / kTerrainTileSize;
		return Float2{ kTerrainUvCosA * u - kTerrainUvSinA * v, kTerrainUvSinA * u + kTerrainUvCosA * v };
	}

	TerrainClipVertex lerpClipVertex(const TerrainClipVertex& a, const TerrainClipVertex& b, float t)
	{
		TerrainClipVertex out;
		out.pos = a.pos + (b.pos - a.pos) * t;
		out.tex = a.tex + (b.tex - a.tex) * t;
		return out;
	}

	float signedAreaXZ(const Array<Vec2>& polygon)
	{
		float area = 0.0f;
		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const Vec2& a = polygon[i];
			const Vec2& b = polygon[(i + 1) % polygon.size()];
			area += static_cast<float>(a.x * b.y - b.x * a.y);
		}
		return area * 0.5f;
	}

	RectF boundsOfPolygon(const Array<Vec2>& polygon)
	{
		if (polygon.isEmpty()) return RectF{};
		float minX = static_cast<float>(polygon[0].x);
		float maxX = static_cast<float>(polygon[0].x);
		float minY = static_cast<float>(polygon[0].y);
		float maxY = static_cast<float>(polygon[0].y);
		for (const auto& p : polygon)
		{
			minX = Min(minX, static_cast<float>(p.x));
			maxX = Max(maxX, static_cast<float>(p.x));
			minY = Min(minY, static_cast<float>(p.y));
			maxY = Max(maxY, static_cast<float>(p.y));
		}
		return RectF{ minX, minY, Max(0.0f, maxX - minX), Max(0.0f, maxY - minY) };
	}

	bool rectIntersects(const RectF& a, const RectF& b)
	{
		return (a.x < (b.x + b.w))
		    && (b.x < (a.x + a.w))
		    && (a.y < (b.y + b.h))
		    && (b.y < (a.y + a.h));
	}

	Array<TerrainClipVertex> clipPolygonByHeight(const Array<TerrainClipVertex>& polygon,
	                                             float planeY, bool keepBelow)
	{
		Array<TerrainClipVertex> out;
		if (polygon.size() < 3) return out;

		auto isInside = [&](const TerrainClipVertex& v)
		{
			return keepBelow
				? (v.pos.y <= planeY + kTerrainClipEpsilon)
				: (v.pos.y >= planeY - kTerrainClipEpsilon);
		};

		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const TerrainClipVertex& curr = polygon[i];
			const TerrainClipVertex& next = polygon[(i + 1) % polygon.size()];
			const bool currInside = isInside(curr);
			const bool nextInside = isInside(next);

			if (currInside && nextInside)
			{
				out << next;
				continue;
			}

			const float dy = static_cast<float>(next.pos.y - curr.pos.y);
			if (Abs(dy) <= kTerrainClipEpsilon)
			{
				if (currInside && !nextInside)
					out << curr;
				continue;
			}

			const float t = Clamp(static_cast<float>((planeY - curr.pos.y) / dy), 0.0f, 1.0f);
			const TerrainClipVertex hit = lerpClipVertex(curr, next, t);

			if (currInside && !nextInside)
			{
				out << curr;
				out << hit;
			}
			else if (!currInside && nextInside)
			{
				out << hit;
				out << next;
			}
		}

		return out;
	}

	Array<TerrainClipVertex> clipPolygonByHalfPlaneXZ(const Array<TerrainClipVertex>& polygon,
	                                                  const Vec2& edgeA, const Vec2& edgeB,
	                                                  bool keepInside)
	{
		Array<TerrainClipVertex> out;
		if (polygon.size() < 3) return out;

		const Vec2 edge = edgeB - edgeA;
		auto signedDistance = [&](const TerrainClipVertex& v)
		{
			const Vec2 p{ static_cast<float>(v.pos.x), static_cast<float>(v.pos.z) };
			return static_cast<float>(edge.x * (p.y - edgeA.y) - edge.y * (p.x - edgeA.x));
		};
		auto isInside = [&](float d)
		{
			return keepInside ? (d >= -kTerrainClipEpsilon) : (d <= kTerrainClipEpsilon);
		};

		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const TerrainClipVertex& curr = polygon[i];
			const TerrainClipVertex& next = polygon[(i + 1) % polygon.size()];
			const float d0 = signedDistance(curr);
			const float d1 = signedDistance(next);
			const bool currInside = isInside(d0);
			const bool nextInside = isInside(d1);

			if (currInside && nextInside)
			{
				out << next;
				continue;
			}

			const float denom = d0 - d1;
			if (Abs(denom) <= kTerrainClipEpsilon)
			{
				if (currInside && !nextInside)
					out << curr;
				continue;
			}

			const float t = Clamp(d0 / denom, 0.0f, 1.0f);
			const TerrainClipVertex hit = lerpClipVertex(curr, next, t);

			if (currInside && !nextInside)
			{
				out << curr;
				out << hit;
			}
			else if (!currInside && nextInside)
			{
				out << hit;
				out << next;
			}
		}

		return out;
	}

	Array<Array<TerrainClipVertex>> subtractConvexPolygonXZ(const Array<TerrainClipVertex>& subject,
	                                                        const Array<Vec2>& clipPolygon)
	{
		Array<Array<TerrainClipVertex>> pending, kept;
		if (subject.size() < 3 || clipPolygon.size() < 3)
		{
			if (subject.size() >= 3) kept << subject;
			return kept;
		}

		pending << subject;
		for (size_t i = 0; i < clipPolygon.size(); ++i)
		{
			const Vec2 a = clipPolygon[i];
			const Vec2 b = clipPolygon[(i + 1) % clipPolygon.size()];
			Array<Array<TerrainClipVertex>> nextPending;

			for (const auto& poly : pending)
			{
				Array<TerrainClipVertex> inside = clipPolygonByHalfPlaneXZ(poly, a, b, true);
				Array<TerrainClipVertex> outside = clipPolygonByHalfPlaneXZ(poly, a, b, false);
				if (outside.size() >= 3) kept << std::move(outside);
				if (inside.size() >= 3) nextPending << std::move(inside);
			}

			pending = std::move(nextPending);
			if (pending.isEmpty()) break;
		}

		return kept;
	}

	Float3 polygonNormal(const Array<TerrainClipVertex>& polygon)
	{
		if (polygon.size() < 3) return Float3{ 0.0f, 1.0f, 0.0f };
		const Vec3 a = polygon[1].pos - polygon[0].pos;
		const Vec3 b = polygon[2].pos - polygon[0].pos;
		const Vec3 n = a.cross(b);
		if (n.lengthSq() <= 1e-10) return Float3{ 0.0f, 1.0f, 0.0f };
		const Vec3 normalized = n.normalized();
		return Float3{ static_cast<float>(normalized.x), static_cast<float>(normalized.y), static_cast<float>(normalized.z) };
	}

	void appendPolygonAsTriangles(const Array<TerrainClipVertex>& polygon,
	                              Array<Vertex3D>& vertices,
	                              Array<TriangleIndex32>& indices)
	{
		if (polygon.size() < 3) return;
		const Float3 normal = polygonNormal(polygon);

		for (size_t i = 1; i + 1 < polygon.size(); ++i)
		{
			const uint32 base = static_cast<uint32>(vertices.size());
			const TerrainClipVertex tri[3] = { polygon[0], polygon[i], polygon[i + 1] };
			for (const auto& v : tri)
			{
				Vertex3D vert;
				vert.pos = Float3{ static_cast<float>(v.pos.x), static_cast<float>(v.pos.y), static_cast<float>(v.pos.z) };
				vert.normal = normal;
				vert.tex = v.tex;
				vertices << vert;
			}
			indices << TriangleIndex32{ base, base + 1, base + 2 };
		}
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

void WorldRenderer::render(World& world, const RoadNetwork& network, const BasicCamera3D& camera)
{
	// アクティブチャンクを近傍優先で回し、地形本体と水面を分けて描画する。
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);
	m_terrainRebuildBudget = Clamp(static_cast<int>(m_pendingTerrainRebuildKeys.size()), 1, 16);

	const Vec3 eye = camera.getEyePosition();
	const int camCx = static_cast<int>(Math::Floor(eye.x / CHUNK_SIZE));
	const int camCz = static_cast<int>(Math::Floor(eye.z / CHUNK_SIZE));
	const Point camChunk{ camCx, camCz };

	const auto& activeChunks = world.getActiveChunks();
	const bool activeChunkOrderChanged = (m_sortedChunks.size() != activeChunks.size())
		|| !std::equal(activeChunks.begin(), activeChunks.end(), m_sortedChunks.begin(), m_sortedChunks.end());

	// ソート済み順序を維持しつつ、カメラ位置やアクティブ集合が変わった時だけ並べ直す
	if (camChunk != m_lastSortChunk || activeChunks.size() != m_lastActiveCount || activeChunkOrderChanged)
	{
		m_sortedChunks = activeChunks;
		m_sortedChunks.sort_by([&](const Chunk* a, const Chunk* b)
		{
			return a->worldOrigin().distanceFromSq(eye) < b->worldOrigin().distanceFromSq(eye);
		});
		m_lastSortChunk  = camChunk;
		m_lastActiveCount = activeChunks.size();
	}

	const auto sceneSize = Scene::Size();
	constexpr float kMargin = 512.0f;

	for (Chunk* chunk : m_sortedChunks)
	{
		if (!chunk) continue;

		// カメラチャンクと周囲8チャンク（計9チャンク）は常に描画
		const int dx = Math::Abs(chunk->coord.x - camCx);
		const int dz = Math::Abs(chunk->coord.y - camCz);
		const bool isNearCamera = (dx <= 1 && dz <= 1);

		// BoundingBox の 8 頂点がすべてスクリーン外なら描画スキップ。
		// 頂点単位で判定するため、連続地形の境界付近が過剰カリングされにくい。
		const Vec3 o = chunk->worldOrigin();
		const double yLo = static_cast<double>(chunk->heightMin);
		const double yHi = static_cast<double>(chunk->heightMax);
		const double cs  = static_cast<double>(CHUNK_SIZE);
		const Float3 corners[8] = {
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z + cs) },
		};
		bool anyVisible = false;
		for (const auto& c : corners)
		{
			const Float3 sp = camera.worldToScreenPoint(c);
			if (sp.z > 0.0f &&
			    sp.x >= -kMargin && sp.x <= sceneSize.x + kMargin &&
			    sp.y >= -kMargin && sp.y <= sceneSize.y + kMargin)
			{
				anyVisible = true;
				break;
			}
		}
		if (!anyVisible && !isNearCamera) continue;

		drawChunk(*chunk, world, network);
	}

	// ---- 水面（y=0 の半透明平面）----
	{
		const ScopedRenderStates3D blend{ BlendState::Default2D };
		const ColorF waterColor = ColorF{ 0.15, 0.35, 0.55, 0.7 }.removeSRGBCurve();
		constexpr double cs = static_cast<double>(CHUNK_SIZE);

		for (const Chunk* chunk : m_sortedChunks)
		{
			if (!chunk) continue;
			// 水面下の地形がないチャンクはスキップ
			if (chunk->heightMin > 0.0f) continue;

			const double ox = static_cast<double>(chunk->coord.x) * cs;
			const double oz = static_cast<double>(chunk->coord.y) * cs;
			const double cx = ox + cs * 0.5;
			const double cz = oz + cs * 0.5;
			Box{ cx, -0.5, cz, cs, 1.0, cs }.draw(waterColor);
		}
	}
}

void WorldRenderer::drawChunk(Chunk& chunk, const World& world, const RoadNetwork& network)
{
	// チャンク単位で地形と建物の GPU キャッシュを更新し、そのまま描画まで完結させる。
	const Key key = chunkCoordToKey(chunk.coord);
	const bool terrainDirty = chunk.meshDirty || m_pendingTerrainRebuildKeys.contains(key);

	if (!m_meshCache.contains(key))
	{
		DBG_LOG(U"[TerrainBool] drawChunk build chunk=({}, {}) dirty={} pending={}"_fmt(
			chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
			m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0));
		// 初回: DynamicMesh を生成して GPU バッファを確保する
		m_meshCache[key] = DynamicMesh{ buildTerrainMeshData(chunk, network) };
		m_pendingTerrainRebuildKeys.erase(key);
		chunk.meshDirty = false;
		rebuildBuildingMeshes(key, chunk, world);
	}
	else if (terrainDirty && m_terrainRebuildBudget > 0)
	{
		DBG_LOG(U"[TerrainBool] drawChunk refill chunk=({}, {}) dirty={} pending={} budget={}"_fmt(
			chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
			m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0, m_terrainRebuildBudget));
		// boolean subtraction でトポロジが変わるため、fill() では古い地形が残ることがある。
		// terrain 更新時は DynamicMesh を作り直して確実に反映する。
		const bool buildingDirty = chunk.meshDirty;
		m_meshCache[key] = DynamicMesh{ buildTerrainMeshData(chunk, network) };
		m_pendingTerrainRebuildKeys.erase(key);
		chunk.meshDirty = false;
		--m_terrainRebuildBudget;
		if (buildingDirty)
			rebuildBuildingMeshes(key, chunk, world);
	}

	// 急斜面では地形メッシュの薄い断面が見えるため両面描画にする
	const ScopedRenderStates3D cullNone{ RasterizerState::SolidCullNone };
	m_meshCache[key].draw(TextureAsset(Asset::Grass), ColorF{ 1.0 }.removeSRGBCurve());
	drawCachedBuildings(key);
}

MeshData WorldRenderer::buildTerrainMeshData(const Chunk& chunk, const RoadNetwork& network)
{
	const Vec3 worldOrigin = chunk.worldOrigin();
	const Array<TerrainSubtractionQuad>& quads = getChunkSubtractionQuads(network, chunk.coord);
	DBG_LOG(U"[TerrainBool] buildTerrainMeshData chunk=({}, {}) quads={}"_fmt(
		chunk.coord.x, chunk.coord.y, quads.size()));

	Array<Vertex3D> vertices;
	Array<TriangleIndex32> indices;
	vertices.reserve(HEIGHT_CELLS * HEIGHT_CELLS * 6);
	indices.reserve(HEIGHT_CELLS * HEIGHT_CELLS * 2);

	auto makeGridVertex = [&](int col, int row)
	{
		const Vec3 pos = worldOrigin + Vec3{
			col * kTerrainCellSize,
			chunk.heightMap[{ col, row }],
			row * kTerrainCellSize
		};
		return TerrainClipVertex{ pos, terrainUvAt(pos) };
	};

	auto processTriangle = [&](const TerrainClipVertex& a, const TerrainClipVertex& b, const TerrainClipVertex& c)
	{
		Array<Array<TerrainClipVertex>> pieces;
		pieces << Array<TerrainClipVertex>{ a, b, c };

		if (!quads.isEmpty())
		{
			RectF triBounds = boundsOfPolygon({
				Vec2{ static_cast<float>(a.pos.x), static_cast<float>(a.pos.z) },
				Vec2{ static_cast<float>(b.pos.x), static_cast<float>(b.pos.z) },
				Vec2{ static_cast<float>(c.pos.x), static_cast<float>(c.pos.z) },
			});

			for (const auto& quad : quads)
			{
				if (!rectIntersects(triBounds, quad.bounds)) continue;

				Array<Array<TerrainClipVertex>> nextPieces;
				for (const auto& piece : pieces)
				{
					Array<TerrainClipVertex> below = clipPolygonByHeight(piece, quad.bedBottomY, true);
					if (below.size() >= 3) nextPieces << std::move(below);

					Array<TerrainClipVertex> above = clipPolygonByHeight(piece, quad.bedBottomY, false);
					if (above.size() >= 3)
					{
						Array<Array<TerrainClipVertex>> kept = subtractConvexPolygonXZ(above, quad.footprint);
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

		for (const auto& poly : pieces)
			appendPolygonAsTriangles(poly, vertices, indices);
	};

	for (int row = 0; row < HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col < HEIGHT_CELLS; ++col)
		{
			const TerrainClipVertex v00 = makeGridVertex(col, row);
			const TerrainClipVertex v10 = makeGridVertex(col + 1, row);
			const TerrainClipVertex v01 = makeGridVertex(col, row + 1);
			const TerrainClipVertex v11 = makeGridVertex(col + 1, row + 1);

			processTriangle(v00, v01, v10);
			processTriangle(v10, v01, v11);
		}
	}

	return MeshData{ vertices, indices };
}

const WorldRenderer::TerrainBooleanSubtractor* WorldRenderer::getTerrainSubtractor(
	const RoadNetwork& network, int edgeId)
{
	if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		return &it->second;

	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || edge->edgeState != EdgeState::Open || edge->useElevation || edge->parts.isEmpty())
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

	const CubicBezier& bez = *bezOpt;
	const int sampleCount = Max(2, static_cast<int>(Math::Ceil(bez.totalLength / kRoadTerrainQuadStep)) + 1);

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
		const float s = bez.totalLength * ft;
		const Vec3 center = bez.positionAt(s);
		float leftOffset = 0.0f;
		float rightOffset = 0.0f;
		if (!calcRoadCrossSectionRange(*edge, ft, leftOffset, rightOffset))
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
		sample.bedBottomY = static_cast<float>(center.y + kRoadSurfaceLift - kRoadBedThickness);
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
			(samples[i].bedBottomY + samples[i + 1].bedBottomY) * 0.5f);
	}

	auto appendNodeCapFootprint = [&](bool isNodeA)
	{
		const int nodeId = isNodeA ? edge->nodeA : edge->nodeB;
		const RoadNode* node = network.getNode(nodeId);
		if (!node) return;

		float roadbedLeft = 0.0f;
		float roadbedRight = 0.0f;
		if (!calcRoadbedRangeAtNode(*edge, isNodeA, roadbedLeft, roadbedRight))
			return;

		const float capRad = isNodeA ? edge->cutoffA : edge->cutoffB;
		constexpr float kOverlap = 0.1f;
		Vec3 capPos, capTan;
		if (isNodeA)
		{
			const float s = Clamp(capRad - kOverlap, 0.0f, bez.totalLength * 0.45f);
			capPos = bez.positionAt(s);
			capTan = bez.tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez.totalLength - capRad + kOverlap,
				bez.totalLength * 0.55f, bez.totalLength);
			capPos = bez.positionAt(s);
			capTan = -bez.tangentAt(s);
		}

		const Vec2 tanXZ{ capTan.x, capTan.z };
		const double tanLen = tanXZ.length();
		if (tanLen < 1e-6) return;
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };
		const Vec3 right = tangentToRight(capTan);

		const Vec3 leftCorner = Vec3{
			capPos.x + right.x * static_cast<double>(roadbedLeft),
			capPos.y,
			capPos.z + right.z * static_cast<double>(roadbedLeft)
		};
		const Vec3 rightCorner = Vec3{
			capPos.x + right.x * static_cast<double>(roadbedRight),
			capPos.y,
			capPos.z + right.z * static_cast<double>(roadbedRight)
		};
		const Vec3 nodeCenter = node->position;

		Array<Vec2> footprint = {
			Vec2{ static_cast<float>(leftCorner.x), static_cast<float>(leftCorner.z) },
			Vec2{ static_cast<float>(nodeCenter.x), static_cast<float>(nodeCenter.z) },
			Vec2{ static_cast<float>(rightCorner.x), static_cast<float>(rightCorner.z) },
		};
		const float bedBottomY = static_cast<float>(
			Max(Max(leftCorner.y, rightCorner.y), nodeCenter.y) + kRoadSurfaceLift - kRoadBedThickness);
		registerSubtractionFootprint(std::move(footprint), bedBottomY);
	};

	appendNodeCapFootprint(true);
	appendNodeCapFootprint(false);

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
	if (!node || node->attachments.size() < 3)
		return nullptr;

	if (node->type != NodeType::Intersection && node->type != NodeType::Diverge)
		return nullptr;

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

	struct NodeEdgeInfo
	{
		Vec3  capTan;
		Vec3  leftCorner;
		Vec3  rightCorner;
		float bedBottomY = 0.0f;
		double angle = 0.0;
	};

	Array<NodeEdgeInfo> infos;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge || edge->useElevation || edge->parts.isEmpty()) continue;
		if (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing) continue;

		const auto bezOpt = network.getBezier(edge->id);
		if (!bezOpt || bezOpt->totalLength <= 0.0f) continue;
		const CubicBezier& bez = *bezOpt;

		const bool isNodeA = (edge->nodeA == nodeId);
		float roadbedLeft = 0.0f;
		float roadbedRight = 0.0f;
		if (!calcRoadbedRangeAtNode(*edge, isNodeA, roadbedLeft, roadbedRight))
			continue;

		const float capRad = isNodeA ? edge->cutoffA : edge->cutoffB;
		constexpr float kOverlap = 0.1f;
		Vec3 capPos, capTan;
		if (isNodeA)
		{
			const float s = Clamp(capRad - kOverlap, 0.0f, bez.totalLength * 0.45f);
			capPos = bez.positionAt(s);
			capTan = bez.tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez.totalLength - capRad + kOverlap,
				bez.totalLength * 0.55f, bez.totalLength);
			capPos = bez.positionAt(s);
			capTan = -bez.tangentAt(s);
		}

		const Vec2 tanXZ{ capTan.x, capTan.z };
		const double tanLen = tanXZ.length();
		if (tanLen < 1e-6) continue;
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };
		const Vec3 right = tangentToRight(capTan);

		NodeEdgeInfo info;
		info.capTan = capTan;
		info.leftCorner = Vec3{
			capPos.x + right.x * static_cast<double>(roadbedLeft),
			capPos.y,
			capPos.z + right.z * static_cast<double>(roadbedLeft)
		};
		info.rightCorner = Vec3{
			capPos.x + right.x * static_cast<double>(roadbedRight),
			capPos.y,
			capPos.z + right.z * static_cast<double>(roadbedRight)
		};
		info.bedBottomY = static_cast<float>(capPos.y + kRoadSurfaceLift - kRoadBedThickness);
		info.angle = Math::Atan2(capTan.z, capTan.x);
		infos << info;
	}

	if (infos.size() < 2)
		return nullptr;

	infos.sort_by([](const NodeEdgeInfo& a, const NodeEdgeInfo& b) { return a.angle < b.angle; });

	const int div = 8;
	const int half = div / 2;
	Array<Array<Vec3>> fillets(static_cast<size_t>(infos.size()));

	for (int i = 0; i < static_cast<int>(infos.size()); ++i)
	{
		const int next = (i + 1) % static_cast<int>(infos.size());
		const NodeEdgeInfo& ei = infos[i];
		const NodeEdgeInfo& en = infos[next];

		const Vec2 p0xz{ ei.leftCorner.x, ei.leftCorner.z };
		const Vec2 p3xz{ en.rightCorner.x, en.rightCorner.z };
		const double dist = (p3xz - p0xz).length();
		const double scale = Max(dist / 3.0, 0.5);
		const Vec2 p1xz = p0xz + Vec2{ -ei.capTan.x, -ei.capTan.z } * scale;
		const Vec2 p2xz = p3xz + Vec2{ -en.capTan.x, -en.capTan.z } * scale;

		Array<Vec3> pts;
		pts.reserve(div + 1);
		for (int k = 0; k <= div; ++k)
		{
			if (k == 0)
			{
				pts << ei.leftCorner;
				continue;
			}
			if (k == div)
			{
				pts << en.rightCorner;
				continue;
			}

			const double t = k / static_cast<double>(div);
			const double mt = 1.0 - t;
			const Vec2 ptXZ = p0xz * (mt * mt * mt)
			                + p1xz * (3.0 * mt * mt * t)
			                + p2xz * (3.0 * mt * t * t)
			                + p3xz * (t * t * t);
			const double y = ei.leftCorner.y + (en.rightCorner.y - ei.leftCorner.y) * k / div;
			pts << Vec3{ ptXZ.x, y, ptXZ.y };
		}
		fillets[static_cast<size_t>(i)] = std::move(pts);
	}

	auto registerTriangle = [&](const Vec3& a, const Vec3& b, const Vec3& c)
	{
		Array<Vec2> tri = {
			Vec2{ static_cast<float>(a.x), static_cast<float>(a.z) },
			Vec2{ static_cast<float>(b.x), static_cast<float>(b.z) },
			Vec2{ static_cast<float>(c.x), static_cast<float>(c.z) },
		};
		const float bedBottomY = static_cast<float>(
			Max(Max(a.y, b.y), c.y) + kRoadSurfaceLift - kRoadBedThickness);
		registerSubtractionFootprint(std::move(tri), bedBottomY);
	};

	for (int i = 0; i < static_cast<int>(infos.size()); ++i)
	{
		const int next = (i + 1) % static_cast<int>(infos.size());
		const Array<Vec3>& fa = fillets[static_cast<size_t>(i)];
		const Array<Vec3>& fb = fillets[static_cast<size_t>(next)];

		for (int k = 0; k < half; ++k)
		{
			const Vec3& p0 = fa[div - k];
			const Vec3& p1 = fa[div - k - 1];
			const Vec3& p2 = fb[k];
			const Vec3& p3 = fb[k + 1];
			registerTriangle(p0, p1, p2);
			registerTriangle(p1, p3, p2);
		}
	}

	if (infos.size() >= 3)
	{
		Array<Vec3> mids;
		for (int i = 0; i < static_cast<int>(infos.size()); ++i)
			mids << fillets[static_cast<size_t>(i)][half];

		while (mids.size() >= 3)
		{
			registerTriangle(mids[0], mids[1], mids[2]);
			mids.erase(mids.begin() + 1);
		}
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

	m_chunkSubtractorPrimed = true;
}

void WorldRenderer::invalidateTerrainChunkKeys(const std::unordered_set<Key>& chunkKeys, bool rebuildImmediately)
{
	if (chunkKeys.empty()) return;

	for (const Key key : chunkKeys)
	{
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
	DBG_LOG(U"[TerrainBool] invalidateMoved node={} attachments={} chunksAfterRadius={}"_fmt(
		nodeId, dirtyEdgeIds.size(), dirtyChunkKeys.size()));
	DBG_LOG(U"[TerrainBool] invalidateMoved radiusChunks={}"_fmt(formatChunkKeySet(dirtyChunkKeys)));

	for (const auto& [cachedEdgeId, subtractor] : m_edgeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (dirtyChunkKeys.contains(key))
			{
				dirtyEdgeIds.insert(cachedEdgeId);
				break;
			}
		}
	}
	for (const auto& [cachedNodeId, subtractor] : m_nodeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (dirtyChunkKeys.contains(key))
			{
				dirtyNodeIds.insert(cachedNodeId);
				break;
			}
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
			DBG_LOG(U"[TerrainBool] invalidateMoved oldEdge edge={} touched={}"_fmt(
				edgeId, formatChunkKeyArray(it->second.touchedChunkKeys)));
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_edgeSubtractorCache.erase(it);
		}
	}
	for (const int dirtyNodeId : dirtyNodeIds)
	{
		if (const auto it = m_nodeSubtractorCache.find(dirtyNodeId); it != m_nodeSubtractorCache.end())
		{
			DBG_LOG(U"[TerrainBool] invalidateMoved oldNode node={} touched={}"_fmt(
				dirtyNodeId, formatChunkKeyArray(it->second.touchedChunkKeys)));
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_nodeSubtractorCache.erase(it);
		}
	}
	DBG_LOG(U"[TerrainBool] invalidateMoved node={} chunksAfterOldTouched={}"_fmt(
		nodeId, dirtyChunkKeys.size()));
	DBG_LOG(U"[TerrainBool] invalidateMoved oldMergedChunks={}"_fmt(formatChunkKeySet(dirtyChunkKeys)));

	for (const int edgeId : dirtyEdgeIds)
	{
		if (const TerrainBooleanSubtractor* updated = getTerrainSubtractor(network, edgeId))
		{
			DBG_LOG(U"[TerrainBool] invalidateMoved newEdge edge={} touched={}"_fmt(
				edgeId, formatChunkKeyArray(updated->touchedChunkKeys)));
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
		}
	}
	for (const int dirtyNodeId : dirtyNodeIds)
	{
		if (const TerrainNodeSubtractor* updated = getTerrainNodeSubtractor(network, dirtyNodeId))
		{
			DBG_LOG(U"[TerrainBool] invalidateMoved newNode node={} touched={}"_fmt(
				dirtyNodeId, formatChunkKeyArray(updated->touchedChunkKeys)));
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
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
	m_meshCache.clear();
	m_chunkSubtractorCache.clear();
	m_edgeSubtractorCache.clear();
	m_nodeSubtractorCache.clear();
	m_pendingTerrainRebuildKeys.clear();
	m_chunkSubtractorPrimed = false;
}

namespace
{
	/// @brief 非 OBJ 建物（Box 描画）の従来高さスケール
	constexpr float kLegacyBoxHeightScale = 5.0f;
	/// @brief モデル TOML に scale が無い場合の既定値
	constexpr float kDefaultModelScale = 1.0f;

	String buildingAssetSubDir(BuildingType type)
	{
		return isResidentialBuildingType(type) ? U"residential" : U"commercial";
	}

	float parseModelScale(const TOMLReader& toml)
	{
		const double s = toml[U"scale"].getOr<double>(
			toml[U"render_scale"].getOr<double>(kDefaultModelScale));
		return static_cast<float>(Max(0.001, s));
	}

	/// @brief 建物 Box メッシュの頂点を中心 (cx, cz) まわりに角度 angle で Y 軸回転する
	/// @details rebuildBuildingMeshes と drawBuildingSilhouette で同じ変換を適用するための共通処理
	void rotateBoxVerticesY(MeshData& box, float cx, float cz, float angle)
	{
		if (angle == 0.0f) return;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (auto& v : box.vertices)
		{
			const float dx = v.pos.x - cx;
			const float dz = v.pos.z - cz;
			v.pos.x = cx + dx * cosA - dz * sinA;
			v.pos.z = cz + dx * sinA + dz * cosA;
			const float nx = v.normal.x;
			const float nz = v.normal.z;
			v.normal.x = nx * cosA - nz * sinA;
			v.normal.z = nx * sinA + nz * cosA;
		}
	}

	/// @brief 住宅 OBJ を全 part 単色で描画する（シルエット用）
	void drawModelSilhouette(Model& model, const Mat4x4& worldMat, const ColorF& color)
	{
		const Transformer3D transform{ worldMat };
		for (const auto& obj : model.objects())
		{
			for (const auto& part : obj.parts)
				part.mesh.draw(color);
		}
	}
}

WorldRenderer::BuildingModelAsset& WorldRenderer::getBuildingModelAsset(BuildingType type, uint8 variant)
{
	// 建物種別ごとの OBJ/TOML を遅延ロードし、以後はモデルとスケールを共有キャッシュで再利用する。
	const uint32 key = (static_cast<uint32>(type) << 8) | static_cast<uint32>(variant);
	auto it = m_buildingModels.find(key);
	if (it != m_buildingModels.end())
	{
		return it->second;
	}

	String stem;
	if (!tryGetBuildingModelStem(type, 0, 0, stem))
	{
		// OBJ 非対応種別は空アセットを返す
		auto [inserted, _] = m_buildingModels.emplace(key, BuildingModelAsset{});
		return inserted->second;
	}

	if (isResidentialBuildingType(type))
	{
		stem = U"residential_{:03d}"_fmt(variant + 1);
	}
	const String subDir = buildingAssetSubDir(type);
	const String path = U"assets/buildings/{}/{}.obj"_fmt(subDir, stem);
	const String tomlPath = U"assets/buildings/{}/{}.toml"_fmt(subDir, stem);

	BuildingModelAsset asset;
	const TOMLReader toml{ tomlPath };
	if (toml)
	{
		asset.scale = parseModelScale(toml);
	}
	else
	{
		Console << U"[WorldRenderer] building TOML load failed: " << tomlPath
		        << U" (scale fallback=" << kDefaultModelScale << U")";
	}

	asset.model = Model{ path };
	if (!asset.model.isEmpty())
	{
		Model::RegisterDiffuseTextures(asset.model, TextureDesc::MippedSRGB);
	}
	else
	{
		Console << U"[WorldRenderer] building model load failed: " << path;
	}

	auto [inserted, _] = m_buildingModels.emplace(key, std::move(asset));
	return inserted->second;
}

Optional<OrientedBox> WorldRenderer::buildingHitBox(const Chunk& chunk, const World& world,
                                                     int col, int row)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return none;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return none;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

	if (isObjBuildingType(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = isResidentialBuildingType(b.type) ? residentialModelIndex(b.type, gx, gz) : 0;
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return none;
		const float yaw = b.angle;

		const Box& lb = asset.model.boundingBox();
		const Vec3 localCenter = lb.center;
		const Vec3 size = lb.size * asset.scale;
		// drawCachedBuildings と同じ Mat4x4::RotateY → translate 変換を再現
		const double cosA = Math::Cos(yaw);
		const double sinA = Math::Sin(yaw);
		const Vec3 worldCenter{
			cx + localCenter.x * asset.scale * cosA + localCenter.z * asset.scale * sinA,
			gy + localCenter.y * asset.scale,
			cz - localCenter.x * asset.scale * sinA + localCenter.z * asset.scale * cosA
		};
		return OrientedBox{ worldCenter, size, Quaternion::RotateY(yaw) };
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return none;
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;
	return OrientedBox{ Vec3{ cx, cy, cz }, Vec3{ footprint, height, footprint },
	                   Quaternion::RotateY(b.angle) };
}

void WorldRenderer::drawBuildingSilhouette(const Chunk& chunk, const World& world,
                                            int col, int row, const ColorF& color)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

	if (isObjBuildingType(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = isResidentialBuildingType(b.type) ? residentialModelIndex(b.type, gx, gz) : 0;
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return;
		const float yaw = b.angle;

		drawModelSilhouette(asset.model,
		                    (Mat4x4::Scale(asset.scale)
		                   * Mat4x4::RotateY(yaw)).translated(cx, gy, cz),
		                    color);
		return;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return;
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

	// rebuildBuildingMeshes と同じパイプライン（MeshData::Box + 頂点手動回転）で描画する
	MeshData box = MeshData::Box(
		Float3{ cx, cy, cz },
		Float3{ footprint, height, footprint });
	rotateBoxVerticesY(box, cx, cz, b.angle);
	Mesh{ box }.draw(color);
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();

	const Vec3 origin = chunk.worldOrigin();

	// 建物種別ごとに MeshData を積み上げる（Box 描画用）
	HashTable<int, MeshData> groups;
	// 住宅 OBJ インスタンス
	Array<BuildingModelInstance> modelInstances;

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			if (b.type == BuildingType::None || b.type == BuildingType::Farmland)
				continue;

			const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

			// 住宅系は OBJ で描画する（地表位置に Y 軸回転のみ適用）
			if (isObjBuildingType(b.type))
			{
				const float gy = world.sampleHeight(cx, cz);
				const int gx = chunk.coord.x * ZONE_CELLS + col;
				const int gz = chunk.coord.y * ZONE_CELLS + row;
				const uint8 variant = isResidentialBuildingType(b.type) ? residentialModelIndex(b.type, gx, gz) : 0;
				BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
				modelInstances.push_back({
					b.type,
					variant,
					Float3{ cx, gy, cz },
					b.angle,
					asset.scale
				});
				continue;
			}

			const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
			if (height <= 0.0f) continue;
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ footprint, height, footprint });

			// 最近傍道路の向きに合わせてY軸回転
			rotateBoxVerticesY(box, cx, cz, b.angle);

			auto& dst = groups[static_cast<int>(b.type)];
			const uint32 offset = static_cast<uint32>(dst.vertices.size());
			dst.vertices.append(box.vertices);
			for (const auto& tri : box.indices)
			{
				dst.indices << TriangleIndex32{
					tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
			}
		}
	}

	auto& batches = m_buildingMeshCache[key];
	batches.clear();
	for (auto& [typeInt, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		batches.push_back({
			buildingColor(static_cast<BuildingType>(typeInt)).removeSRGBCurve(),
			Mesh{ meshData }
		});
	}

	m_buildingModelCache[key] = std::move(modelInstances);
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
	if (const auto it = m_buildingMeshCache.find(key); it != m_buildingMeshCache.end())
	{
		for (const auto& batch : it->second)
		{
			batch.mesh.draw(batch.color);
		}
	}

	if (const auto it = m_buildingModelCache.find(key); it != m_buildingModelCache.end())
	{
		auto* self = const_cast<WorldRenderer*>(this);
		for (const auto& inst : it->second)
		{
			BuildingModelAsset& asset = self->getBuildingModelAsset(inst.type, inst.modelVariant);
			if (asset.model.isEmpty()) continue;

			const Mat4x4 worldMat = (Mat4x4::Scale(inst.scale)
			                       * Mat4x4::RotateY(inst.angle))
				.translated(inst.pos.x, inst.pos.y, inst.pos.z);
			const auto& materials = asset.model.materials();
			for (const auto& obj : asset.model.objects())
			{
				const Transformer3D transform{ worldMat };
				obj.draw(materials);
			}
		}
	}
}
