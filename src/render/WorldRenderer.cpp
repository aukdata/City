#include "WorldRenderer.hpp"
#include <Siv3D/Profiler.hpp>

void WorldRenderer::render(World& world)
{
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);

	// 手前チャンクから順に描画（Front-to-Back で Early-Z 最適化）
	const Vec3 eye{ Graphics3D::GetEyePosition() };

	auto chunks = world.getActiveChunks();
	chunks.sort_by([&](const Chunk* a, const Chunk* b)
	{
		return a->worldOrigin().distanceFromSq(eye) < b->worldOrigin().distanceFromSq(eye);
	});

	for (Chunk* chunk : chunks)
	{
		if (chunk)
			drawChunk(*chunk, world);
	}
}

void WorldRenderer::drawChunk(Chunk& chunk, const World& world)
{
	const Key key = chunkKey(chunk.coord);

	if (!m_meshCache.contains(key))
	{
		// 初回: DynamicMesh を生成して GPU バッファを確保する
		m_meshCache[key] = DynamicMesh{ buildTerrainMeshData(chunk) };
		chunk.dirty = false;
		rebuildBuildingMeshes(key, chunk, world);
	}
	else if (chunk.dirty)
	{
		// 差分更新: GPU バッファを作り直さず頂点データだけ書き換える（W100 回避）
		m_meshCache[key].fill(buildTerrainMeshData(chunk));
		chunk.dirty = false;
		rebuildBuildingMeshes(key, chunk, world);
	}

	// 急斜面では地形メッシュの薄い断面が見えるため両面描画にする
	const ScopedRenderStates3D cullNone{ RasterizerState::SolidCullNone };
	if (m_grassTexture.isEmpty())
		m_grassTexture = Texture{ U"assets/textures/grass.png", TextureDesc::MippedSRGB };
	m_meshCache[key].draw(m_grassTexture, ColorF{ 1.0 }.removeSRGBCurve());
	drawCachedBuildings(key);
}

MeshData WorldRenderer::buildTerrainMeshData(const Chunk& chunk)
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	const Vec3 worldOrigin   = chunk.worldOrigin();

	const int gridSize = HEIGHT_CELLS + 1;

	// ワールド空間での1テクスチャタイルのサイズ [m]（= CHUNK_SIZE / kGrassTile = 1024 / 80 ≈ 12.8m）
	// チャンク境界をまたいで連続したUVにするため、ローカルUVではなくワールド座標から計算する
	constexpr float kTileSize = static_cast<float>(CHUNK_SIZE) / (16.0f * 5.0f);

	// 整数倍でないわずかな固定角度回転でグリッド感を崩す
	constexpr float kRotDeg = 13.5f;
	constexpr float kCosA = 0.97237f;  // cos(13.5°)
	constexpr float kSinA = 0.23345f;  // sin(13.5°)

	Array<Vertex3D> vertices;
	vertices.reserve(gridSize * gridSize);

	for (int row = 0; row < gridSize; ++row)
	{
		for (int col = 0; col < gridSize; ++col)
		{
			const float height = chunk.heightMap[{ col, row }];
			const Vec3 wpos = worldOrigin + Vec3{
				col * cellSize,
				height,
				row * cellSize
			};

			// 隣頂点の高さから中央差分で法線を計算する
			const int c0 = Max(col - 1, 0), c2 = Min(col + 1, HEIGHT_CELLS);
			const int r0 = Max(row - 1, 0), r2 = Min(row + 1, HEIGHT_CELLS);
			const float dhx = chunk.heightMap[{ c2, row }] - chunk.heightMap[{ c0, row }];
			const float dhz = chunk.heightMap[{ col, r2 }] - chunk.heightMap[{ col, r0 }];
			// 接線ベクトル T_x=(2c,dhx,0), T_z=(0,dhz,2c) の外積 → normalize(-dhx, 2c, -dhz)
			const Float3 n = Float3{ -dhx, 2.0f * cellSize, -dhz }.normalized();

			// ワールド座標からUVを計算することでチャンク境界をまたいで連続させる
			const float u = static_cast<float>(wpos.x) / kTileSize;
			const float v = static_cast<float>(wpos.z) / kTileSize;

			Vertex3D vert;
			vert.pos    = Float3{ static_cast<float>(wpos.x), static_cast<float>(wpos.y), static_cast<float>(wpos.z) };
			vert.normal = n;
			vert.tex    = Float2{ kCosA * u - kSinA * v, kSinA * u + kCosA * v };

			vertices << vert;
		}
	}

	Array<TriangleIndex32> indices;
	indices.reserve(HEIGHT_CELLS * HEIGHT_CELLS * 2);

	for (int row = 0; row < HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col < HEIGHT_CELLS; ++col)
		{
			const uint32 i00 = static_cast<uint32>(row       * gridSize + col    );
			const uint32 i10 = static_cast<uint32>(row       * gridSize + col + 1);
			const uint32 i01 = static_cast<uint32>((row + 1) * gridSize + col    );
			const uint32 i11 = static_cast<uint32>((row + 1) * gridSize + col + 1);

			// Siv3D (DirectX) の 3D メッシュは CW = 表面
			indices << TriangleIndex32{ i00, i01, i10 };
			indices << TriangleIndex32{ i10, i01, i11 };
		}
	}

	return MeshData{ vertices, indices };
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float footprint = 10.0f;

	const Vec3 origin = chunk.worldOrigin();

	// 建物種別ごとに MeshData を積み上げる
	HashTable<int, MeshData> groups;

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			if (b.type == BuildingType::None || b.type == BuildingType::Farmland)
				continue;

			const float height = buildingHeight(b.type, b.stage);
			if (height <= 0.0f) continue;

			const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ footprint, height, footprint });

			// 最近傍道路の向きに合わせてY軸回転
			if (b.angle != 0.0f)
			{
				const float cosA = Math::Cos(b.angle);
				const float sinA = Math::Sin(b.angle);
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
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
	const auto it = m_buildingMeshCache.find(key);
	if (it == m_buildingMeshCache.end()) return;
	for (const auto& batch : it->second)
	{
		batch.mesh.draw(batch.color);
	}
}

float WorldRenderer::buildingHeight(BuildingType type, uint8 stage)
{
	switch (type)
	{
	case BuildingType::Detached:       return 4.0f  + stage * 2.0f;
	case BuildingType::LowApartment:   return 10.0f + stage * 4.0f;
	case BuildingType::MidApartment:   return 24.0f + stage * 8.0f;
	case BuildingType::HighApartment:  return 48.0f + stage * 12.0f;
	case BuildingType::Shop:           return 4.0f  + stage * 1.5f;
	case BuildingType::Office:         return 16.0f + stage * 10.0f;
	case BuildingType::Factory:        return 8.0f  + stage * 4.0f;
	case BuildingType::ParkBuilding:   return 0.5f;
	case BuildingType::PublicFacility: return 10.0f + stage * 3.0f;
	case BuildingType::Parking:        return 2.5f;
	default:                           return 0.0f;
	}
}

ColorF WorldRenderer::buildingColor(BuildingType type)
{
	switch (type)
	{
	case BuildingType::Detached:       return ColorF{ 0.90, 0.82, 0.68 };
	case BuildingType::LowApartment:   return ColorF{ 0.65, 0.75, 0.90 };
	case BuildingType::MidApartment:   return ColorF{ 0.45, 0.58, 0.82 };
	case BuildingType::HighApartment:  return ColorF{ 0.30, 0.42, 0.75 };
	case BuildingType::Shop:           return ColorF{ 0.95, 0.78, 0.30 };
	case BuildingType::Office:         return ColorF{ 0.70, 0.75, 0.80 };
	case BuildingType::Factory:        return ColorF{ 0.50, 0.48, 0.46 };
	case BuildingType::ParkBuilding:   return ColorF{ 0.30, 0.70, 0.35 };
	case BuildingType::PublicFacility: return ColorF{ 0.80, 0.60, 0.85 };
	case BuildingType::Parking:        return ColorF{ 0.55, 0.55, 0.55 };
	default:                           return ColorF{ 0.60, 0.60, 0.60 };
	}
}

void WorldRenderer::markDirty(Point chunkCoord)
{
	// DynamicMesh はキャッシュを消去せず dirty フラグ経由で fill() 更新するため、
	// ここでは何もしない（呼び出し側との互換性のために残す）
	(void)chunkCoord;
}
