#include "WorldRenderer.hpp"

void WorldRenderer::render(const World& world)
{
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (chunk)
			drawChunk(*chunk);
	}
}

void WorldRenderer::drawChunk(const Chunk& chunk)
{
	const Key key = chunkKey(chunk.coord);

	// dirty またはキャッシュにない場合はメッシュを再生成
	if (chunk.dirty || !m_meshCache.contains(key))
	{
		m_meshCache[key] = buildTerrainMesh(chunk);
	}

	m_meshCache[key].draw(ColorF{ 0.35, 0.55, 0.25 });
	drawBuildings(chunk);
}

Mesh WorldRenderer::buildTerrainMesh(const Chunk& chunk)
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	const Vec3 worldOrigin   = chunk.worldOrigin();

	const int gridSize = HEIGHT_CELLS + 1;

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

			Vertex3D v;
			v.pos    = Float3{ static_cast<float>(wpos.x), static_cast<float>(wpos.y), static_cast<float>(wpos.z) };
			v.normal = Float3{ 0.0f, 1.0f, 0.0f };
			v.tex    = Float2{ col / static_cast<float>(HEIGHT_CELLS), row / static_cast<float>(HEIGHT_CELLS) };

			vertices << v;
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

			// 三角形1: (row,col) → (row,col+1) → (row+1,col)
			indices << TriangleIndex32{ i00, i10, i01 };
			// 三角形2: (row,col+1) → (row+1,col+1) → (row+1,col)
			indices << TriangleIndex32{ i10, i11, i01 };
		}
	}

	return Mesh{ MeshData{ vertices, indices } };
}

void WorldRenderer::drawBuildings(const Chunk& chunk)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS; // 16 m
	constexpr float footprint = 10.0f;  // 建物の水平サイズ [m]

	const Vec3 origin = chunk.worldOrigin();

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			if (b.type == BuildingType::None) continue;

			// 建物の高さ（種別・成長段階で変化）
			float height = 0.0f;
			ColorF color;
			switch (b.type)
			{
			case BuildingType::Detached:
				height = 4.0f + b.stage * 2.0f;
				color  = ColorF{ 0.90, 0.82, 0.68 };
				break;
			case BuildingType::LowApartment:
				height = 10.0f + b.stage * 4.0f;
				color  = ColorF{ 0.65, 0.75, 0.90 };
				break;
			case BuildingType::MidApartment:
				height = 24.0f + b.stage * 8.0f;
				color  = ColorF{ 0.45, 0.58, 0.82 };
				break;
			case BuildingType::HighApartment:
				height = 48.0f + b.stage * 12.0f;
				color  = ColorF{ 0.30, 0.42, 0.75 };
				break;
			case BuildingType::Shop:
				height = 4.0f + b.stage * 1.5f;
				color  = ColorF{ 0.95, 0.78, 0.30 };
				break;
			case BuildingType::Office:
				height = 16.0f + b.stage * 10.0f;
				color  = ColorF{ 0.70, 0.75, 0.80 };
				break;
			case BuildingType::Factory:
				height = 8.0f + b.stage * 4.0f;
				color  = ColorF{ 0.50, 0.48, 0.46 };
				break;
			case BuildingType::Farmland:
				// 農地は地面と同化させる（高さなし）
				continue;
			case BuildingType::ParkBuilding:
				height = 0.5f;
				color  = ColorF{ 0.30, 0.70, 0.35 };
				break;
			case BuildingType::PublicFacility:
				height = 10.0f + b.stage * 3.0f;
				color  = ColorF{ 0.80, 0.60, 0.85 };
				break;
			case BuildingType::Parking:
				height = 2.5f;
				color  = ColorF{ 0.55, 0.55, 0.55 };
				break;
			default:
				continue;
			}

			// セル中心のワールド座標
			const double cx = origin.x + (col + 0.5) * cellSize;
			const double cz = origin.z + (row + 0.5) * cellSize;
			const double cy = height * 0.5;

			Box{ cx, cy, cz, footprint, height, footprint }.draw(color);
		}
	}
}

void WorldRenderer::markDirty(Point chunkCoord)
{
	m_meshCache.erase(chunkKey(chunkCoord));
}
