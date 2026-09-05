#include "WorldRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadGeometry.hpp"
#include <Siv3D/Profiler.hpp>
#include <Siv3D/ViewFrustum.hpp>
#include <algorithm>

namespace
{
	constexpr float kTerrainCellSize      = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	constexpr float kRoadTerrainQuadStep  = 24.0f;
	constexpr float kRoadTerrainRelief    = 0.06f;
	constexpr float kTerrainClipEpsilon   = 1e-4f;
	constexpr float kChunkSizeF           = static_cast<float>(CHUNK_SIZE);
	constexpr float kTerrainTileSize      = 96.0f;
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
	m_meshCache[key].draw(TextureAsset(Asset::Ground), ColorF{ 0.58, 0.58, 0.50 }.removeSRGBCurve());
	drawCachedBuildings(key);
}

MeshData WorldRenderer::buildTerrainMeshData(const Chunk& chunk, const RoadNetwork& network)
{
	const Vec3 worldOrigin = chunk.worldOrigin();
	const Array<TerrainSubtractionQuad>& quads = getChunkSubtractionQuads(network, chunk.coord);
	constexpr size_t kMaxDetailedSubtractionQuads = 512;
	const bool useDetailedSubtraction = (quads.size() <= kMaxDetailedSubtractionQuads);
	DBG_LOG(U"[TerrainBool] buildTerrainMeshData chunk=({}, {}) quads={} detailed={}"_fmt(
		chunk.coord.x, chunk.coord.y, quads.size(), useDetailedSubtraction));

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

		if (useDetailedSubtraction && !quads.isEmpty())
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
		sample.bedBottomY = static_cast<float>(center.y + kRoadSurfaceLift - kRoadTerrainRelief);
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
			Max(Max(leftCorner.y, rightCorner.y), nodeCenter.y) + kRoadSurfaceLift - kRoadTerrainRelief);
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
		info.bedBottomY = static_cast<float>(capPos.y + kRoadSurfaceLift - kRoadTerrainRelief);
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
			Max(Max(a.y, b.y), c.y) + kRoadSurfaceLift - kRoadTerrainRelief);
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
	m_meshCache.clear();
	m_chunkSubtractorCache.clear();
	m_edgeSubtractorCache.clear();
	m_nodeSubtractorCache.clear();
	m_pendingTerrainRebuildKeys.clear();
	m_chunkSubtractorPrimed = false;
}

namespace
{
	/// @brief 非 OBJ 建物（Box 描画）の高さスケール。buildingHeight() はメートル基準。
	constexpr float kLegacyBoxHeightScale = 1.0f;
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

	void appendMeshData(MeshData& dst, const MeshData& src)
	{
		const uint32 offset = static_cast<uint32>(dst.vertices.size());
		dst.vertices.append(src.vertices);
		for (const auto& tri : src.indices)
		{
			dst.indices << TriangleIndex32{ tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
		}
	}

	uint32 cellVisualHash(Point chunkCoord, int col, int row, uint32 salt)
	{
		uint32 value = static_cast<uint32>(chunkCoord.x * 73856093)
			^ static_cast<uint32>(chunkCoord.y * 19349663)
			^ static_cast<uint32>(col * 83492791)
			^ static_cast<uint32>(row * 2654435761u)
			^ salt;
		value ^= value >> 16;
		value *= 0x7FEB352Du;
		value ^= value >> 15;
		value *= 0x846CA68Bu;
		value ^= value >> 16;
		return value;
	}

	void appendRotatedBox(MeshData& dst, float cx, float cy, float cz, float sx, float sy, float sz, float angle)
	{
		MeshData box = MeshData::Box(Float3{ cx, cy, cz }, Float3{ sx, sy, sz });
		rotateBoxVerticesY(box, cx, cz, angle);
		appendMeshData(dst, box);
	}

	int landPatchMaterialKey(LandPatchType type, uint64 seed)
	{
		switch (type)
		{
		case LandPatchType::ParcelAsphalt: return 117;
		case LandPatchType::ParcelGravel:  return 118;
		case LandPatchType::GardenSoil:    return ((seed >> 4) & 1u) ? 112 : 101;
		case LandPatchType::Beach:        return 111;
		case LandPatchType::PaddyField:   return 110;
		case LandPatchType::FarmField:    return ((seed >> 6) & 1u) ? 113 : 101;
		case LandPatchType::Seawall:      return 117;
		default:                    return 100;
		}
	}

	void appendLandPatchSurface(MeshData& dst, const World& world, const LandPatch& patch)
	{
		if (patch.polygon.size() < 3) return;
		Array<TerrainClipVertex> polygon;
		polygon.reserve(patch.polygon.size());
		for (const Vec2& p : patch.polygon)
		{
			const float y = static_cast<float>(world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.y))) + patch.elevationOffset;
			const Vec3 pos{ p.x, y, p.y };
			polygon << TerrainClipVertex{ pos, terrainUvAt(pos) };
		}
		if (signedAreaXZ(patch.polygon) < 0.0f)
		{
			polygon.reverse();
		}
		appendPolygonAsTriangles(polygon, dst.vertices, dst.indices);
	}

	void appendLandPatchMesh(HashTable<int, MeshData>& groups, const World& world, const LandPatch& patch)
	{
		const bool drawSurface = (patch.type != LandPatchType::ParcelAsphalt
			&& patch.type != LandPatchType::ParcelGravel
			&& patch.type != LandPatchType::GardenSoil
			&& patch.type != LandPatchType::Seawall);
		if (drawSurface)
		{
			MeshData& surface = groups[landPatchMaterialKey(patch.type, patch.materialVariant)];
			appendLandPatchSurface(surface, world, patch);
		}
		const RectF bounds = boundsOfPolygon(patch.polygon);
		const float cx = static_cast<float>(bounds.x + bounds.w * 0.5);
		const float cz = static_cast<float>(bounds.y + bounds.h * 0.5);
		const float baseY = static_cast<float>(world.sampleHeight(cx, cz)) + patch.elevationOffset;

		if (patch.sourceParcelKey >= 0 && patch.polygon.size() >= 4
			&& (patch.type == LandPatchType::ParcelAsphalt || patch.type == LandPatchType::ParcelGravel || patch.type == LandPatchType::GardenSoil))
		{
			const Vec2 a = patch.polygon[0];
			const Vec2 b = patch.polygon[1];
			const Vec2 c = patch.polygon[2];
			const Vec2 d = patch.polygon[3];
			const Vec2 frontMid = (a + b) * 0.5;
			const Vec2 backMid = (c + d) * 0.5;
			Vec2 along = b - a;
			const float frontageLen = static_cast<float>(along.length());
			if (frontageLen > 0.6f)
			{
				along /= frontageLen;
				Vec2 inward = backMid - frontMid;
				if (inward.lengthSq() > 1e-6f) inward.normalize();
				const float apronW = Min(frontageLen * 0.92f, 10.5f);
				const float apronD = 2.20f;
				const Vec2 apronCenter = frontMid + inward * (apronD * 0.55f);
				const float angle = static_cast<float>(std::atan2(along.y, along.x));
				appendRotatedBox(groups[(patch.type == LandPatchType::GardenSoil) ? 114 : 100],
					static_cast<float>(apronCenter.x), baseY + 0.060f, static_cast<float>(apronCenter.y),
					apronW, 0.035f, apronD, angle);
				const uint32 hash = static_cast<uint32>(patch.materialVariant);
				const float openingW = 2.2f + static_cast<float>((hash >> 5) % 5u) * 0.22f;
				const float fenceSpan = Max(0.0f, frontageLen - openingW);
				if (fenceSpan > 1.2f)
				{
					const Vec2 left = frontMid - along * (openingW * 0.5f + fenceSpan * 0.25f) + inward * 0.18f;
					const Vec2 right = frontMid + along * (openingW * 0.5f + fenceSpan * 0.25f) + inward * 0.18f;
					appendRotatedBox(groups[102], static_cast<float>(left.x), baseY + 0.25f, static_cast<float>(left.y),
						fenceSpan * 0.45f, 0.50f, 0.16f, angle);
					appendRotatedBox(groups[102], static_cast<float>(right.x), baseY + 0.25f, static_cast<float>(right.y),
						fenceSpan * 0.45f, 0.50f, 0.16f, angle);
					appendRotatedBox(groups[117], static_cast<float>(frontMid.x - along.x * openingW * 0.48f), baseY + 0.32f,
						static_cast<float>(frontMid.y - along.y * openingW * 0.48f), 0.22f, 0.64f, 0.22f, angle);
					appendRotatedBox(groups[117], static_cast<float>(frontMid.x + along.x * openingW * 0.48f), baseY + 0.32f,
						static_cast<float>(frontMid.y + along.y * openingW * 0.48f), 0.22f, 0.64f, 0.22f, angle);
				}
			}
		}

		if (patch.type == LandPatchType::Seawall)
		{
			appendRotatedBox(groups[117], cx, baseY + 0.70f, cz,
				static_cast<float>(Max(2.4, bounds.w * 1.02)), 1.40f,
				static_cast<float>(Max(1.8, bounds.h * 0.52)), 0.0f);
		}
		else if (patch.type == LandPatchType::FarmField || patch.type == LandPatchType::PaddyField)
		{
			const int ridgeKey = (patch.type == LandPatchType::PaddyField) ? 110 : 101;
			const int borderKey = (patch.type == LandPatchType::PaddyField) ? 114 : 113;
			Vec2 along{ 1.0f, 0.0f };
			if (patch.polygon.size() >= 2)
			{
				along = patch.polygon[1] - patch.polygon[0];
				if (along.lengthSq() <= 1e-6f) along = Vec2{ 1.0f, 0.0f };
				else along.normalize();
			}
			const Vec2 lateral{ -along.y, along.x };
			const float angle = static_cast<float>(std::atan2(along.y, along.x));
			const uint32 hash = static_cast<uint32>(patch.materialVariant);
			const Vec2 frontMid = (patch.polygon[0] + patch.polygon[1]) * 0.5;
			const Vec2 backMid = (patch.polygon[3] + patch.polygon[4]) * 0.5;
			const Vec2 fieldMid = (frontMid + backMid) * 0.5;
			const float fieldLength = static_cast<float>(Max(3.0, bounds.w * 0.84));
			const float fieldDepth = static_cast<float>(Max(3.0, bounds.h * 0.76));
			appendRotatedBox(groups[borderKey], static_cast<float>(frontMid.x), baseY + 0.052f, static_cast<float>(frontMid.y),
				fieldLength, 0.070f, 0.26f, angle);
			appendRotatedBox(groups[borderKey], static_cast<float>(backMid.x), baseY + 0.050f, static_cast<float>(backMid.y),
				fieldLength * 0.92f, 0.065f, 0.23f, angle);
			for (const float side : { -1.0f, 1.0f })
			{
				const Vec2 sideMid = fieldMid + along * side * fieldLength * 0.46f;
				appendRotatedBox(groups[borderKey], static_cast<float>(sideMid.x), baseY + 0.048f, static_cast<float>(sideMid.y),
					fieldDepth, 0.060f, 0.22f, angle + static_cast<float>(90.0_deg));
			}
			appendRotatedBox(groups[borderKey], static_cast<float>(fieldMid.x), baseY + 0.046f, static_cast<float>(fieldMid.y),
				fieldLength * 0.78f, 0.055f, 0.18f, angle);
			const int ridgeCount = (patch.type == LandPatchType::PaddyField) ? 8 : 10;
			for (int i = 0; i < ridgeCount; ++i)
			{
				const float offset = (static_cast<float>(i) - (ridgeCount - 1) * 0.5f) * fieldDepth / Max(1.0f, static_cast<float>(ridgeCount - 1)) * 0.86f
					+ (static_cast<float>((hash >> (i * 5)) & 15u) / 15.0f - 0.5f) * fieldDepth * 0.018f;
				const Vec2 p = fieldMid + lateral * offset;
				appendRotatedBox(groups[ridgeKey], static_cast<float>(p.x), baseY + 0.055f, static_cast<float>(p.y),
					fieldLength * 0.82f, 0.060f, 0.12f, angle);
			}
			if (patch.type == LandPatchType::PaddyField)
			{
				for (const float side : { -0.33f, 0.33f })
				{
					const Vec2 channel = fieldMid + along * side * fieldLength;
					appendRotatedBox(groups[110], static_cast<float>(channel.x), baseY + 0.060f, static_cast<float>(channel.y),
						fieldDepth * 0.78f, 0.060f, 0.24f, angle + static_cast<float>(90.0_deg));
				}
			}
		}
	}
	void appendGableRoof(MeshData& dst, float cx, float baseY, float cz,
	                     float sx, float depth, float roofH, float angle, bool ridgeAlongX)
	{
		const float hx = sx * 0.5f;
		const float hz = depth * 0.5f;
		const float ridgeX = ridgeAlongX ? hx : 0.0f;
		const float ridgeZ = ridgeAlongX ? 0.0f : hz;
		Array<Float3> local;
		if (ridgeAlongX)
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ -ridgeX, baseY + roofH, 0.0f }, Float3{ ridgeX, baseY + roofH, 0.0f }
			};
		}
		else
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ 0.0f, baseY + roofH, -ridgeZ }, Float3{ 0.0f, baseY + roofH, ridgeZ }
			};
		}

		MeshData md;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (const Float3& p : local)
		{
			const float x = ridgeAlongX ? p.x : p.x;
			const float z = p.z;
			Vertex3D v;
			v.pos = Float3{ cx + x * cosA - z * sinA, p.y, cz + x * sinA + z * cosA };
			v.normal = Float3{ 0.0f, 1.0f, 0.0f };
			v.tex = Float2{ 0.0f, 0.0f };
			md.vertices << v;
		}
		md.indices << TriangleIndex32{ 0, 1, 4 };
		md.indices << TriangleIndex32{ 1, 5, 4 };
		md.indices << TriangleIndex32{ 1, 2, 5 };
		md.indices << TriangleIndex32{ 2, 3, 5 };
		md.indices << TriangleIndex32{ 3, 4, 5 };
		md.indices << TriangleIndex32{ 3, 0, 4 };
		appendMeshData(dst, md);
	}

	float targetBuildingModelFootprint(BuildingType type)
	{
		switch (type)
		{
		case BuildingType::Detached:      return 7.5f;
		case BuildingType::LowApartment:  return 8.5f;
		case BuildingType::MidApartment:  return 9.5f;
		case BuildingType::HighApartment: return 10.5f;
		case BuildingType::Shop:          return 8.0f;
		case BuildingType::Office:        return 10.5f;
		default:                          return buildingFootprintXZ() * 0.78f;
		}
	}

	float targetBuildingModelHeight(BuildingType type)
	{
		return Max(1.0f, buildingHeight(type));
	}

	float normalizedObjScale(BuildingType type, const Box& localBounds, float assetScale)
	{
		float scale = assetScale;
		const float localFootprint = static_cast<float>(Max(localBounds.size.x, localBounds.size.z));
		if (localFootprint > 0.001f)
		{
			const float currentFootprint = localFootprint * scale;
			const float targetFootprint = targetBuildingModelFootprint(type);
			if (currentFootprint > targetFootprint)
			{
				scale *= targetFootprint / currentFootprint;
			}
		}

		const float localHeight = static_cast<float>(localBounds.size.y);
		if (localHeight > 0.001f)
		{
			const float currentHeight = localHeight * scale;
			const float targetHeight = targetBuildingModelHeight(type);
			if (currentHeight > targetHeight)
			{
				scale *= targetHeight / currentHeight;
			}
		}
		return scale;
	}
	float boxBuildingFootprintScale(BuildingType type, int gx, int gz)
	{
		const uint32 hash = static_cast<uint32>(gx) * 73856093u ^ static_cast<uint32>(gz) * 19349663u;
		const float variation = 0.86f + static_cast<float>(hash % 29u) * (0.24f / 28.0f);
		switch (type)
		{
		case BuildingType::Factory:        return 1.28f * variation;
		case BuildingType::PublicFacility: return 1.16f * variation;
		case BuildingType::Parking:        return 1.05f * variation;
		case BuildingType::ParkBuilding:   return 0.92f * variation;
		default:                           return variation;
		}
	}

	ColorF detailColorForKey(int key)
	{
		switch (key)
		{
		case 100: return ColorF{ 0.45, 0.43, 0.38 };
		case 101: return ColorF{ 0.34, 0.50, 0.24 };
		case 102: return ColorF{ 0.62, 0.59, 0.52 };
		case 103: return ColorF{ 0.42, 0.39, 0.34 };
		case 104: return ColorF{ 0.46, 0.53, 0.31 };
		case 105: return ColorF{ 0.32, 0.24, 0.18 };
		case 106: return ColorF{ 0.22, 0.36, 0.22 };
		case 107: return ColorF{ 0.72, 0.74, 0.73 };
		case 108: return ColorF{ 0.35, 0.39, 0.43 };
		case 109: return ColorF{ 0.30, 0.30, 0.28 };
		case 110: return ColorF{ 0.22, 0.48, 0.54 };
		case 111: return ColorF{ 0.58, 0.53, 0.39 };
		case 112: return ColorF{ 0.68, 0.20, 0.16 };
		case 113: return ColorF{ 0.30, 0.56, 0.24 };
		case 114: return ColorF{ 0.62, 0.55, 0.34 };
		case 115: return ColorF{ 0.22, 0.21, 0.19 };
		case 116: return ColorF{ 0.16, 0.23, 0.25 };
		case 117: return ColorF{ 0.36, 0.36, 0.34 };
		case 118: return ColorF{ 0.49, 0.47, 0.39 };
		default: return ColorF{ 0.60, 0.60, 0.60 };
		}
	}

	void appendUrbanLotDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                           int col, int row, const Building& building, float cx, float cz,
	                           float cellSize)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(building.type));
		const float angle = building.angle;
		const float baseY = world.sampleHeight(cx, cz);
		const float lotSize = cellSize * (0.99f + static_cast<float>((hash >> 3) % 3u) * 0.005f);
		const bool commercialLike = (building.type == BuildingType::Shop || building.type == BuildingType::Office
			|| building.type == BuildingType::Factory || building.type == BuildingType::PublicFacility);
		const int surfaceKey = (building.type == BuildingType::Parking || commercialLike) ? 100 : (((hash >> 9) % 100u < 8u) ? 101 : (((hash >> 15) & 1u) ? 114 : 100));
		const float surfaceSize = (building.type == BuildingType::Parking) ? lotSize : (commercialLike ? lotSize * 0.52f : lotSize * 0.34f);
		appendRotatedBox(groups[surfaceKey], cx, baseY + 0.020f, cz, surfaceSize, 0.035f, surfaceSize * (commercialLike ? 0.62f : 0.46f), angle);

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (building.type == BuildingType::Parking)
		{
			appendRotatedBox(groups[109], cx, baseY + 0.09f, cz, lotSize * 0.92f, 0.08f, lotSize * 0.74f, angle);
			for (int i = -1; i <= 1; ++i)
			{
				const Vec2 p = worldOffset(static_cast<float>(i) * lotSize * 0.22f,
					((hash >> (10 + i + 1)) & 1u) ? lotSize * 0.08f : -lotSize * 0.13f);
				appendRotatedBox(groups[(i == 0) ? 108 : 107], static_cast<float>(p.x), baseY + 0.30f,
					static_cast<float>(p.y), 1.65f, 0.42f, 3.35f, angle);
			}
			return;
		}

		if (building.type == BuildingType::ParkBuilding)
		{
			appendRotatedBox(groups[101], cx, baseY + 0.08f, cz, lotSize * 0.78f, 0.10f, lotSize * 0.78f, angle);
			for (int i = 0; i < 3; ++i)
			{
				const Vec2 p = worldOffset((static_cast<float>((hash >> (i * 4)) % 9u) - 4.0f) * 0.9f,
					(static_cast<float>((hash >> (i * 5 + 7)) % 9u) - 4.0f) * 0.9f);
				appendRotatedBox(groups[105], static_cast<float>(p.x), baseY + 0.70f, static_cast<float>(p.y), 0.24f, 1.40f, 0.24f, angle);
				appendRotatedBox(groups[106], static_cast<float>(p.x), baseY + 1.65f, static_cast<float>(p.y), 1.45f, 1.25f, 1.45f, angle + static_cast<float>(45.0_deg));
			}
			return;
		}

		const float frontageZ = -lotSize * 0.43f;
		if (commercialLike)
		{
			appendRotatedBox(groups[109], cx, baseY + 0.055f, cz, lotSize * 0.54f, 0.035f, lotSize * 0.24f, angle);
			const Vec2 sign = worldOffset(lotSize * 0.34f, frontageZ);
			appendRotatedBox(groups[115], static_cast<float>(sign.x), baseY + 1.15f, static_cast<float>(sign.y), 0.16f, 2.30f, 0.16f, angle);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + 2.45f, static_cast<float>(sign.y), 1.20f, 0.60f, 0.12f, angle);
		}
		else
		{
			appendRotatedBox(groups[101], cx, baseY + 0.045f, cz, lotSize * 0.36f, 0.030f, lotSize * 0.15f, angle);
			const Vec2 car = worldOffset(((hash >> 11) & 1u) ? lotSize * 0.25f : -lotSize * 0.25f, lotSize * 0.30f);
			if (((hash >> 6) % 100u) < 52u)
			{
				appendRotatedBox(groups[100], static_cast<float>(car.x), baseY + 0.08f, static_cast<float>(car.y), 2.15f, 0.05f, 4.05f, angle);
				appendRotatedBox(groups[((hash >> 14) & 1u) ? 107 : 108], static_cast<float>(car.x), baseY + 0.32f,
					static_cast<float>(car.y), 1.55f, 0.44f, 3.10f, angle);
			}
			const Vec2 approach = worldOffset(0.0f, frontageZ * 0.54f);
			appendRotatedBox(groups[100], static_cast<float>(approach.x), baseY + 0.082f, static_cast<float>(approach.y),
				lotSize * 0.22f, 0.045f, lotSize * 0.60f, angle);
			if (((hash >> 27) % 100u) < 34u)
			{
				const Vec2 bin = worldOffset(-lotSize * 0.35f, frontageZ + 0.12f);
				appendRotatedBox(groups[117], static_cast<float>(bin.x), baseY + 0.28f, static_cast<float>(bin.y), 0.70f, 0.56f, 0.42f, angle);
			}
		}

		if ((hash % 100u) < 92u)
		{
			const float side = ((hash >> 8) & 1u) ? 1.0f : -1.0f;
			const Vec2 fence = worldOffset(side * lotSize * 0.38f,
				(static_cast<float>((hash >> 12) % 7u) - 3.0f) * 0.65f);
			appendRotatedBox(groups[102], static_cast<float>(fence.x), baseY + 0.42f, static_cast<float>(fence.y),
				0.24f, 0.84f, lotSize * 0.60f, angle);
		}

		if (((hash >> 5) % 100u) < 58u)
		{
			const float side = ((hash >> 17) & 1u) ? 1.0f : -1.0f;
			const Vec2 tree = worldOffset(side * lotSize * 0.36f,
				lotSize * (0.20f + static_cast<float>((hash >> 21) % 18u) * 0.012f));
			appendRotatedBox(groups[105], static_cast<float>(tree.x), baseY + 0.80f, static_cast<float>(tree.y), 0.28f, 1.60f, 0.28f, angle);
			appendRotatedBox(groups[106], static_cast<float>(tree.x), baseY + 1.82f, static_cast<float>(tree.y), 1.65f, 1.45f, 1.65f, angle + static_cast<float>(45.0_deg));
		}

		if (((hash >> 20) % 100u) < 62u)
		{
			const float side = ((hash >> 24) & 1u) ? 1.0f : -1.0f;
			const Vec2 pole = worldOffset(side * lotSize * 0.47f, frontageZ);
			appendRotatedBox(groups[115], static_cast<float>(pole.x), baseY + 2.55f, static_cast<float>(pole.y), 0.18f, 5.10f, 0.18f, angle);
			appendRotatedBox(groups[115], static_cast<float>(pole.x), baseY + 4.65f, static_cast<float>(pole.y), 2.20f, 0.10f, 0.10f, angle);
		}
	}
	void appendFarmlandDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                           int col, int row, float cx, float cz, float cellSize)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, 0xA6B4C893u);
		const float angle = ((hash & 1u) ? 0.0f : static_cast<float>(90.0_deg))
			+ static_cast<float>((static_cast<int>((hash >> 8) % 7u) - 3) * 0.015f);
		const float baseY = world.sampleHeight(cx, cz);
		const float fieldW = cellSize * (0.78f + static_cast<float>(hash % 18u) * 0.010f);
		const float fieldD = cellSize * (0.56f + static_cast<float>((hash >> 16) % 24u) * 0.010f);
		const int fieldKey = ((hash >> 5) % 100u < 42u) ? 113 : (((hash >> 10) & 1u) ? 104 : 114);
		appendRotatedBox(groups[fieldKey], cx, baseY + 0.025f, cz, fieldW, 0.05f, fieldD, angle);

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (int i = -3; i <= 3; ++i)
		{
			const float lx = static_cast<float>(i) * fieldW * 0.135f;
			const float px = cx + lx * cosA;
			const float pz = cz + lx * sinA;
			appendRotatedBox(groups[(i == 0 && ((hash >> 20) & 1u)) ? 110 : 101], px, baseY + 0.065f, pz,
				0.18f, 0.045f, fieldD * 0.98f, angle);
		}
		for (int i = -1; i <= 1; ++i)
		{
			const float lz = static_cast<float>(i) * fieldD * 0.29f;
			const float px = cx - lz * sinA;
			const float pz = cz + lz * cosA;
			appendRotatedBox(groups[114], px, baseY + 0.055f, pz, fieldW * 0.94f, 0.035f, 0.16f, angle);
		}

		if (((hash >> 24) % 100u) < 34u)
		{
			const float px = cx + fieldW * 0.48f * cosA;
			const float pz = cz + fieldW * 0.48f * sinA;
			appendRotatedBox(groups[110], px, baseY + 0.05f, pz, 0.42f, 0.04f, fieldD, angle);
		}
	}
	void appendBoxBuildingDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                              int col, int row, BuildingType type, float cx, float cz,
	                              float footprint, float height, float angle)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(type) ^ 0x712A4C3Du);
		const float baseY = world.sampleHeight(cx, cz);
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (!isObjBuildingType(type))
		{
			appendRotatedBox(groups[103], cx, baseY + height + 0.08f, cz,
			                 footprint * 0.82f, 0.16f, footprint * 0.82f, angle);
		}

		if (type == BuildingType::Parking || type == BuildingType::ParkBuilding)
		{
			return;
		}

		const bool residential = isResidentialBuildingType(type);
		const bool shop = (type == BuildingType::Shop);
		const bool officeLike = (type == BuildingType::Office || type == BuildingType::PublicFacility);
		const int floorCount = Max(1, static_cast<int>(Floor(height / (residential ? 2.8f : 3.2f))));
		const int visibleFloors = Min(floorCount, residential ? 4 : 7);
		const float frontZ = -footprint * 0.515f;
		const float backZ = footprint * 0.515f;
		const float sideX = footprint * 0.515f;

		for (int floor = 0; floor < visibleFloors; ++floor)
		{
			const float y = baseY + 1.15f + static_cast<float>(floor) * (residential ? 2.65f : 3.0f);
			if (y > baseY + height - 0.55f) break;
			const int windowKey = ((hash >> (floor + 3)) & 1u) ? 116 : 102;
			const float rowWidth = footprint * (residential ? 0.20f : 0.16f);
			for (int w = -1; w <= 1; ++w)
			{
				const float lx = static_cast<float>(w) * footprint * 0.23f;
				const Vec2 front = worldOffset(lx, frontZ);
				appendRotatedBox(groups[windowKey], static_cast<float>(front.x), y, static_cast<float>(front.y),
					rowWidth, residential ? 0.56f : 0.76f, 0.10f, angle);
			}
			if (residential && floor > 0)
			{
				const Vec2 balcony = worldOffset(0.0f, frontZ - 0.18f);
				appendRotatedBox(groups[117], static_cast<float>(balcony.x), y - 0.20f, static_cast<float>(balcony.y),
					footprint * 0.68f, 0.12f, 0.28f, angle);
			}
			if (!residential)
			{
				const Vec2 left = worldOffset(-sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(left.x), y, static_cast<float>(left.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
				const Vec2 right = worldOffset(sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(right.x), y, static_cast<float>(right.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
			}
		}

		if (residential)
		{
			const bool ridgeAlongX = ((hash >> 23) & 1u) != 0u;
			const float roofBase = baseY + Max(2.7f, height - (type == BuildingType::Detached ? 1.05f : 0.55f));
			appendGableRoof(groups[103], cx, roofBase, cz, footprint * 0.96f, footprint * 0.86f,
				(type == BuildingType::Detached ? 1.15f : 0.55f), angle, ridgeAlongX);
			const Vec2 eave = worldOffset(0.0f, frontZ - 0.12f);
			appendRotatedBox(groups[103], static_cast<float>(eave.x), baseY + Min(height, 4.8f), static_cast<float>(eave.y),
				footprint * 0.84f, 0.12f, 0.42f, angle);
			const Vec2 unit = worldOffset(sideX + 0.16f, footprint * 0.22f);
			appendRotatedBox(groups[117], static_cast<float>(unit.x), baseY + 1.20f, static_cast<float>(unit.y),
				0.42f, 0.42f, 0.22f, angle);

			if (type == BuildingType::Detached)
			{
				const float wingSide = ((hash >> 4) & 1u) ? 1.0f : -1.0f;
				const Vec2 wing = worldOffset(wingSide * footprint * 0.42f, footprint * 0.10f);
				appendRotatedBox(groups[static_cast<int>(type)], static_cast<float>(wing.x), baseY + 1.15f, static_cast<float>(wing.y),
					footprint * 0.34f, 2.30f, footprint * 0.54f, angle);
				appendGableRoof(groups[103], static_cast<float>(wing.x), baseY + 2.30f, static_cast<float>(wing.y),
					footprint * 0.40f, footprint * 0.62f, 0.62f, angle, !ridgeAlongX);
				const Vec2 shed = worldOffset(-wingSide * footprint * 0.46f, footprint * 0.38f);
				appendRotatedBox(groups[117], static_cast<float>(shed.x), baseY + 0.58f, static_cast<float>(shed.y),
					1.25f, 1.16f, 1.70f, angle);
				appendRotatedBox(groups[103], static_cast<float>(shed.x), baseY + 1.22f, static_cast<float>(shed.y),
					1.45f, 0.16f, 1.95f, angle);
			}
		}

		if (shop)
		{
			const Vec2 sign = worldOffset(0.0f, frontZ - 0.10f);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + Min(height * 0.74f, 3.15f), static_cast<float>(sign.y),
				footprint * 0.72f, 0.55f, 0.12f, angle);
			const Vec2 awning = worldOffset(0.0f, frontZ - 0.32f);
			appendRotatedBox(groups[118], static_cast<float>(awning.x), baseY + 2.25f, static_cast<float>(awning.y),
				footprint * 0.78f, 0.16f, 0.62f, angle);
			for (int d = -1; d <= 1; ++d)
			{
				const Vec2 door = worldOffset(static_cast<float>(d) * footprint * 0.22f, frontZ - 0.07f);
				appendRotatedBox(groups[116], static_cast<float>(door.x), baseY + 1.10f, static_cast<float>(door.y),
					footprint * 0.16f, 1.35f, 0.10f, angle);
			}
		}

		if (officeLike || type == BuildingType::Factory)
		{
			const Vec2 rear = worldOffset(0.0f, backZ + 0.10f);
			appendRotatedBox(groups[117], static_cast<float>(rear.x), baseY + Max(1.5f, height * 0.42f), static_cast<float>(rear.y),
				footprint * 0.36f, 0.38f, 0.24f, angle);
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
	if (isResidentialBuildingType(type))
	{
		stem = U"residential_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Shop)
	{
		stem = U"shop_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Office)
	{
		stem = U"office_{:03d}"_fmt(variant + 1);
	}
	else
	{
		// OBJ 非対応種別は空アセットを返す
		auto [inserted, _] = m_buildingModels.emplace(key, BuildingModelAsset{});
		return inserted->second;
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
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return none;
		const float yaw = b.angle;

		const Box& lb = asset.model.boundingBox();
		const Vec3 localCenter = lb.center;
		const float modelScale = normalizedObjScale(b.type, lb, asset.scale);
		const Vec3 size = lb.size * modelScale;
		// drawCachedBuildings と同じ Mat4x4::RotateY → translate 変換を再現
		const double cosA = Math::Cos(yaw);
		const double sinA = Math::Sin(yaw);
		const Vec3 worldCenter{
			cx + localCenter.x * modelScale * cosA + localCenter.z * modelScale * sinA,
			gy + localCenter.y * modelScale,
			cz - localCenter.x * modelScale * sinA + localCenter.z * modelScale * cosA
		};
		return OrientedBox{ worldCenter, size, Quaternion::RotateY(yaw) };
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return none;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;
	return OrientedBox{ Vec3{ cx, cy, cz }, Vec3{ visualFootprint, height, visualFootprint },
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
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return;
		const float yaw = b.angle;

		drawModelSilhouette(asset.model,
		                    (Mat4x4::Scale(normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale))
		                   * Mat4x4::RotateY(yaw)).translated(cx, gy, cz),
		                    color);
		return;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

	// rebuildBuildingMeshes と同じパイプライン（MeshData::Box + 頂点手動回転）で描画する
	MeshData box = MeshData::Box(
		Float3{ cx, cy, cz },
		Float3{ visualFootprint, height, visualFootprint });
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

	for (const LandPatch& patch : chunk.landPatches)
	{
		appendLandPatchMesh(groups, world, patch);
	}

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			const float cellCenterX = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cellCenterZ = static_cast<float>(origin.z + (row + 0.5) * cellSize);
			if (b.type == BuildingType::None)
			{
				continue;
			}

			const float cx = cellCenterX + b.offsetX;
			const float cz = cellCenterZ + b.offsetZ;

			if (b.type == BuildingType::Farmland)
			{
				continue;
			}
			appendUrbanLotDetails(groups, chunk, world, col, row, b, cx, cz, cellSize);

			// 住宅系は OBJ で描画する（地表位置に Y 軸回転のみ適用）
			if (isObjBuildingType(b.type))
			{
				const float gy = world.sampleHeight(cx, cz);
				const int gx = chunk.coord.x * ZONE_CELLS + col;
				const int gz = chunk.coord.y * ZONE_CELLS + row;
				const uint8 variant = buildingModelVariant(b.type, gx, gz);
				BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
				const float modelScale = normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale);
				const Box& bounds = asset.model.boundingBox();
				const float modelFootprint = Max(footprint * 0.72f,
					static_cast<float>(Max(bounds.size.x, bounds.size.z)) * modelScale);
				const float modelHeight = Max(1.0f, static_cast<float>(bounds.size.y) * modelScale);
				appendBoxBuildingDetails(groups, chunk, world, col, row, b.type, cx, cz,
				                         modelFootprint, modelHeight, b.angle);
				modelInstances.push_back({
					b.type,
					variant,
					Float3{ cx, gy, cz },
					b.angle,
					modelScale
				});
				continue;
			}

			const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
			if (height <= 0.0f) continue;
			const int gx = chunk.coord.x * ZONE_CELLS + col;
			const int gz = chunk.coord.y * ZONE_CELLS + row;
			const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ visualFootprint, height, visualFootprint });

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
			appendBoxBuildingDetails(groups, chunk, world, col, row, b.type, cx, cz,
			                         visualFootprint, height, b.angle);
		}
	}

	auto& batches = m_buildingMeshCache[key];
	batches.clear();
	for (auto& [typeInt, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		const ColorF color = (typeInt >= 100)
			? detailColorForKey(typeInt)
			: buildingColor(static_cast<BuildingType>(typeInt));
		batches.push_back({
			color.removeSRGBCurve(),
			Mesh{ meshData }
		});
	}

	m_buildingModelCache[key] = std::move(modelInstances);
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
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

	if (const auto it = m_buildingMeshCache.find(key); it != m_buildingMeshCache.end())
	{
		for (const auto& batch : it->second)
		{
			batch.mesh.draw(batch.color);
		}
	}
}