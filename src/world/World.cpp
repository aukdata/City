#include "../gen/GenerationSettings.hpp"
#include "World.hpp"
#include <cmath>
#include <thread>
#include <atomic>

void World::reserveChunks()
{
	// 全チャンクを先に確保して座標だけ割り当て、後続処理は固定インデックスで参照できるようにする。
	m_chunks.reserve(WORLD_CHUNKS * WORLD_CHUNKS);
	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
			m_chunks.emplace_back(Point{ cx, cy });
}

void World::installChunkDirect(Point coord, HeightMapResult&& hmr)
{
	// 生成済み地形結果を指定チャンクへ直接注入し、高さ範囲と開発可否の初期状態も揃える。
	if (!isValidCoord(coord)) return;

	Chunk& chunk = m_chunks[coordToIndex(coord)];
	chunk.heightMap = std::move(hmr.heightMap);
	chunk.heightMin = hmr.heightMin;
	chunk.heightMax = hmr.heightMax;
	chunk.isUrbanizationArea = true;
}

void World::update(Vec3 cameraWorldPos)
{
	// カメラ中心チャンクが変わった時だけ active 範囲を張り直し、描画・更新対象キャッシュを更新する。
	const Point newChunk = worldToChunkCoord(cameraWorldPos);

	if (newChunk == m_cameraChunk)
		return;

	m_cameraChunk = newChunk;
	bool activeChanged = false;

	for (auto& chunk : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			const int dx = chunk.coord.x - m_cameraChunk.x;
			const int dy = chunk.coord.y - m_cameraChunk.y;
			if (Abs(dx) > ACTIVE_RANGE || Abs(dy) > ACTIVE_RANGE)
			{
				chunk.state = ChunkState::Sleeping;
				activeChanged = true;
			}
		}
	}

	for (int dy = -ACTIVE_RANGE; dy <= ACTIVE_RANGE; ++dy)
	{
		for (int dx = -ACTIVE_RANGE; dx <= ACTIVE_RANGE; ++dx)
		{
			const Point coord{ m_cameraChunk.x + dx, m_cameraChunk.y + dy };
			if (Chunk* chunk = getChunk(coord))
			{
				if (chunk->state != ChunkState::Active)
					activeChanged = true;
				chunk->state = ChunkState::Active;
			}
		}
	}

	if (activeChanged)
		rebuildActiveChunkCache();
}

const Chunk* World::getChunk(Point coord) const
{
	if (!isValidCoord(coord) || static_cast<size_t>(coordToIndex(coord))>=m_chunks.size()) return nullptr;
	const Chunk& chunk = m_chunks[coordToIndex(coord)];
	if (chunk.heightMap.isEmpty()) return nullptr;
	return &chunk;
}

Chunk* World::getChunk(Point coord)
{
	if (!isValidCoord(coord) || static_cast<size_t>(coordToIndex(coord))>=m_chunks.size()) return nullptr;
	Chunk& chunk = m_chunks[coordToIndex(coord)];
	if (chunk.heightMap.isEmpty()) return nullptr;
	return &chunk;
}

void World::rebuildActiveChunkCache()
{
	// Active 状態のチャンク参照だけを配列化し、毎フレームの走査対象を絞る。
	m_activeChunks.clear();
	m_activeChunksConst.clear();
	for (auto& chunk : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			m_activeChunks << &chunk;
			m_activeChunksConst << &chunk;
		}
	}
}

const Array<Chunk*>& World::getActiveChunks()
{
	return m_activeChunks;
}

const Array<const Chunk*>& World::getActiveChunks() const
{
	return m_activeChunksConst;
}

float World::sampleHeight(float wx, float wz) const
{
	const int cx = static_cast<int>(Math::Floor(wx / CHUNK_SIZE));
	const int cz = static_cast<int>(Math::Floor(wz / CHUNK_SIZE));
	const Chunk* chunk = getChunk({ cx, cz });
	if (!chunk)
		return 0.0f;
	return chunk->getHeight(wx, wz);
}

void World::setGenerationParams(uint64 seed, float mapWidth, float mapDepth)
{
	const double kCentralMountainThreshold=GenerationSettings::get().terrain_centralMountainThreshold, kCentralLakeThreshold=GenerationSettings::get().terrain_centralLakeThreshold, kCentralBayThreshold=GenerationSettings::get().terrain_centralBayThreshold;
	const double kFeatureInlandOffset=GenerationSettings::get().terrain_featureInlandOffset, kFeatureInlandSpread=GenerationSettings::get().terrain_featureInlandSpread, kFeatureLateralSpread=GenerationSettings::get().terrain_featureLateralSpread;
	const double kRangeOffsetMin=GenerationSettings::get().terrain_rangeOffsetMin, kRangeOffsetSpread=GenerationSettings::get().terrain_rangeOffsetSpread, kRangeWidthMin=GenerationSettings::get().terrain_rangeWidthMin, kRangeWidthSpread=GenerationSettings::get().terrain_rangeWidthSpread;
	// 地形生成で参照する乱数種とマップ寸法をまとめて差し替え、Perlin も同じ種で再初期化する。
	m_seed     = seed;
	m_mapWidth = mapWidth;
	m_mapDepth = mapDepth;
	m_perlin   = PerlinNoise{ seed };
	m_rivers=RiverNetwork{};
	uint64 state = seed;
	const auto random = [&]()
	{
		state += 0x9e3779b97f4a7c15ULL;
		uint64 value = state;
		value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
		value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
		return static_cast<double>((value ^ (value >> 31)) >> 11) / 9007199254740992.0;
	};
	const double angle = random() * Math::TwoPi;
	m_landAxis = {Cos(angle), Sin(angle)};
	m_bayCenter = (random() - .5) * Min(mapWidth, mapDepth) * GenerationSettings::get().terrain_bayLateralSpread;
	m_terrainPhase = random() * Math::TwoPi;
	const double central=random();
	m_centralLandform=central<kCentralMountainThreshold ? CentralLandform::Mountain : (central<kCentralLakeThreshold ? CentralLandform::Lake
		: (central<kCentralBayThreshold ? CentralLandform::Bay : CentralLandform::Plain));
	m_featureCenter={kFeatureInlandOffset+random()*kFeatureInlandSpread,(random()-.5)*kFeatureLateralSpread};
	m_rangeOffset=kRangeOffsetMin+random()*kRangeOffsetSpread;
	m_rangeWidth=kRangeWidthMin+random()*kRangeWidthSpread;
	m_flankSign=random()<.5 ? -1 : 1;
	if (m_centralLandform==CentralLandform::Bay) { m_bayCenter=m_featureCenter.y*Min(mapWidth,mapDepth); }
	buildMacroTerrain();
}

// ─────────────────────────────────────────────────────────────────────────────
// バイオームベース地形生成
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 地形帯の境界で高さと傾きが連続になる補間。
	double transition(double distance, double width)
	{
		const double t = Clamp(distance / width, 0.0, 1.0);
		return t * t * (3 - 2 * t);
	}
}

double World::lakeInfluence(double inlandAxis, double alongAxis, double scale) const
{
	const double kShoreIrregularity=GenerationSettings::get().terrain_shoreIrregularity, kShoreNoiseScale=GenerationSettings::get().terrain_shoreNoiseScale, kShoreBlendWidth=GenerationSettings::get().terrain_shoreBlendWidth;
	if (m_centralLandform!=CentralLandform::Lake) { return 0; }
	const Vec2 kLakeRadius{GenerationSettings::get().terrain_lakeRadiusX,GenerationSettings::get().terrain_lakeRadiusZ};
	const double x=(inlandAxis/scale-m_featureCenter.x)/kLakeRadius.x;
	const double z=(alongAxis/scale-m_featureCenter.y)/kLakeRadius.y;
	const double edge=1-std::sqrt(x*x+z*z)+kShoreIrregularity*m_perlin.noise2D(inlandAxis/(scale*kShoreNoiseScale),alongAxis/(scale*kShoreNoiseScale));
	return transition(edge,kShoreBlendWidth);
}

void World::computeRawBiomeParams(float wx, float wz, float& outBase, float& outAmp) const
{
	// Horizontal dimensions below are fractions of the shorter map side; heights are metres.
	const double kFullReliefMapSize=GenerationSettings::get().terrain_fullReliefMapSize;
	const double kLargeWarpScale=GenerationSettings::get().terrain_largeWarpScale, kLargeWarpAmplitude=GenerationSettings::get().terrain_largeWarpAmplitude;
	const double kSmallWarpScale=GenerationSettings::get().terrain_smallWarpScale, kSmallWarpAmplitude=GenerationSettings::get().terrain_smallWarpAmplitude, kSmallWarpNoiseOffset=GenerationSettings::get().terrain_smallWarpNoiseOffset;
	const double kMainBayWidth=GenerationSettings::get().terrain_mainBayWidth, kSecondaryBayOffset=GenerationSettings::get().terrain_secondaryBayOffset, kSecondaryBayWidth=GenerationSettings::get().terrain_secondaryBayWidth, kSecondaryBayDepth=GenerationSettings::get().terrain_secondaryBayDepth;
	const double kCapeScale=GenerationSettings::get().terrain_capeScale, kCapeAmplitude=GenerationSettings::get().terrain_capeAmplitude;
	const double kInletScale=GenerationSettings::get().terrain_inletScale, kInletAmplitude=GenerationSettings::get().terrain_inletAmplitude, kInletNoiseOffset=GenerationSettings::get().terrain_inletNoiseOffset;
	const double kShoreDetailScale=GenerationSettings::get().terrain_shoreDetailScale, kShoreDetailAmplitude=GenerationSettings::get().terrain_shoreDetailAmplitude, kShoreDetailNoiseOffset=GenerationSettings::get().terrain_shoreDetailNoiseOffset;
	const double kSeaDepth=GenerationSettings::get().terrain_seaDepth, kSeaShelfWidth=GenerationSettings::get().terrain_seaShelfWidth, kCoastBlendWidth=GenerationSettings::get().terrain_coastBlendWidth;
	const double kFirstRangeWander=GenerationSettings::get().terrain_firstRangeWander, kFirstRangeWavelength=GenerationSettings::get().terrain_firstRangeWavelength;
	const double kSideRangeOffset=GenerationSettings::get().terrain_sideRangeOffset, kSideRangeWander=GenerationSettings::get().terrain_sideRangeWander, kSideRangeWavelength=GenerationSettings::get().terrain_sideRangeWavelength, kSideRangePhase=GenerationSettings::get().terrain_sideRangePhase;
	const double kValleyOffset=GenerationSettings::get().terrain_valleyOffset, kValleyWander=GenerationSettings::get().terrain_valleyWander, kValleyWavelength=GenerationSettings::get().terrain_valleyWavelength, kValleyWidth=GenerationSettings::get().terrain_valleyWidth;
	const double kPassOffset=GenerationSettings::get().terrain_passOffset, kPassWidth=GenerationSettings::get().terrain_passWidth, kValleyReduction=GenerationSettings::get().terrain_valleyReduction, kPassReduction=GenerationSettings::get().terrain_passReduction;
	const double kRidgeFloor=GenerationSettings::get().terrain_ridgeFloor, kRidgeVariation=GenerationSettings::get().terrain_ridgeVariation, kRidgeNoiseScale=GenerationSettings::get().terrain_ridgeNoiseScale, kRidgeNoiseOffset=GenerationSettings::get().terrain_ridgeNoiseOffset;
	const double kFirstRangeHeight=GenerationSettings::get().terrain_firstRangeHeight, kSideRangeHeight=GenerationSettings::get().terrain_sideRangeHeight, kSideRangeWidth=GenerationSettings::get().terrain_sideRangeWidth, kRangeOverlap=GenerationSettings::get().terrain_rangeOverlap;
	const double kCentralMountainHeight=GenerationSettings::get().terrain_centralMountainHeight, kCentralMountainWidth=GenerationSettings::get().terrain_centralMountainWidth, kCentralMountainLength=GenerationSettings::get().terrain_centralMountainLength;
	const double kBroadPlainScale=GenerationSettings::get().terrain_broadPlainScale, kLocalPlainScale=GenerationSettings::get().terrain_localPlainScale, kLocalPlainNoiseOffset=GenerationSettings::get().terrain_localPlainNoiseOffset;
	const double kPlainFloor=GenerationSettings::get().terrain_plainFloor, kInlandGradient=GenerationSettings::get().terrain_inlandGradient, kLakeBed=GenerationSettings::get().terrain_lakeBed, kMountainDetail=GenerationSettings::get().terrain_mountainDetail;
	const double scale=Min(m_mapWidth,m_mapDepth),reliefScale=Min(1.0,scale/kFullReliefMapSize);
	const Vec2 p{wx-m_mapWidth*.5,wz-m_mapDepth*.5};
	const double u=p.dot(m_landAxis),v=p.dot(Vec2{-m_landAxis.y,m_landAxis.x});
	// Warp the coast in two dimensions so small valleys branch off the larger bays.
	const double kCoastOffset=GenerationSettings::get().terrain_coastOffset, kUsualBayDepth=GenerationSettings::get().terrain_usualBayDepth, kCentralBayDepth=GenerationSettings::get().terrain_centralBayDepth;
	const double shoreV=v+scale*kLargeWarpAmplitude*m_perlin.noise2D(u/(scale*kLargeWarpScale),v/(scale*kLargeWarpScale))
		+scale*kSmallWarpAmplitude*m_perlin.noise2D(u/(scale*kSmallWarpScale)+kSmallWarpNoiseOffset,v/(scale*kSmallWarpScale));
	const double bay=std::exp(-Square((shoreV-m_bayCenter)/(scale*kMainBayWidth)));
	const double secondBay=std::exp(-Square((shoreV+scale*kSecondaryBayOffset)/(scale*kSecondaryBayWidth)));
	const double bayDepth=m_centralLandform==CentralLandform::Bay ? kCentralBayDepth : kUsualBayDepth;
	const double coast=scale*(kCoastOffset+bayDepth*bay+kSecondaryBayDepth*secondBay)
		+scale*kCapeAmplitude*Sin(shoreV/(scale*kCapeScale)+m_terrainPhase)
		+scale*kInletAmplitude*m_perlin.noise2D(shoreV/(scale*kInletScale),kInletNoiseOffset)
		+scale*kShoreDetailAmplitude*m_perlin.noise2D(shoreV/(scale*kShoreDetailScale),kShoreDetailNoiseOffset);
	const double inland=u-coast;
	if (inland<0)
	{
		outBase=static_cast<float>(-kSeaDepth*reliefScale*(1-std::exp(inland/(scale*kSeaShelfWidth))));
		outAmp=0; return;
	}
	// The two mountain belts usually occupy the inland and lateral margins, with gaps rather than a border wall.
	const double firstAxis=scale*(m_rangeOffset+kFirstRangeWander*Sin(v/(scale*kFirstRangeWavelength)+m_terrainPhase));
	const double secondAxis=scale*(m_flankSign*kSideRangeOffset+kSideRangeWander*Sin(u/(scale*kSideRangeWavelength)+m_terrainPhase+kSideRangePhase));
	const auto ridge=[](double across,double width)
	{
		const double distance=Abs(across)/width;
		return distance<1 ? .5+.5*Cos(distance*Math::Pi) : 0;
	};
	const double valleyAxis=scale*kValleyOffset*Sin(m_terrainPhase)+scale*kValleyWander*Sin(u/(scale*kValleyWavelength)+m_terrainPhase);
	const double valley=std::exp(-Square((v-valleyAxis)/(scale*kValleyWidth)));
	const double pass=std::exp(-Square((u-scale*kPassOffset)/(scale*kPassWidth)));
	const double variation=kRidgeFloor+kRidgeVariation*m_perlin.noise2D0_1(v/(scale*kRidgeNoiseScale),kRidgeNoiseOffset);
	const double first=kFirstRangeHeight*ridge(u-firstAxis,scale*m_rangeWidth)*(1-kValleyReduction*valley)*variation;
	const double second=kSideRangeHeight*ridge(v-secondAxis,scale*kSideRangeWidth)*(1-kPassReduction*pass)*variation;
	double mountain=Max(first,second)+kRangeOverlap*Min(first,second);
	if (m_centralLandform==CentralLandform::Mountain)
	{
		const double central=kCentralMountainHeight*std::exp(-Square((u/scale-m_featureCenter.x)/kCentralMountainWidth)-Square((v/scale-m_featureCenter.y)/kCentralMountainLength));
		mountain=Max(mountain,central);
	}
	mountain*=reliefScale;
	const double coastBlend=transition(inland,scale*kCoastBlendWidth);
	// Long undulations and shorter terraces replace the near-planar inland ramp. Fine noise remains subdued.
	const double kBroadPlainRelief=GenerationSettings::get().terrain_broadPlainRelief, kLocalPlainRelief=GenerationSettings::get().terrain_localPlainRelief, kPlainDetail=GenerationSettings::get().terrain_plainDetail;
	const double broad=m_perlin.noise2D0_1(wx/(scale*kBroadPlainScale),wz/(scale*kBroadPlainScale));
	const double local=m_perlin.noise2D0_1(wx/(scale*kLocalPlainScale)+kLocalPlainNoiseOffset,wz/(scale*kLocalPlainScale));
	const double plain=kPlainFloor+inland*kInlandGradient+reliefScale*(kBroadPlainRelief*broad+kLocalPlainRelief*local);
	const double lake=lakeInfluence(u,v,scale);
	outBase=static_cast<float>(Math::Lerp((plain+mountain)*coastBlend,kLakeBed*reliefScale,lake));
	outAmp=static_cast<float>((kPlainDetail+mountain*kMountainDetail)*coastBlend*(1-lake));
}

/// @brief 重い海岸・山脈ノイズは共有格子に一度だけ評価し、16m地表と地図は補間で参照する。
void World::buildMacroTerrain()
{
	const auto& config = GenerationSettings::get();
	const int columns = Max(2, static_cast<int>(Ceil(m_mapWidth / config.terrain_macroSampleStep)));
	const int rows = Max(2, static_cast<int>(Ceil(m_mapDepth / config.terrain_macroSampleStep)));
	m_macroStepX = m_mapWidth / columns;
	m_macroStepZ = m_mapDepth / rows;
	m_macroTerrain = Grid<Float2>(columns + 1, rows + 1);
	Grid<float> noise(columns + 1, rows + 1);
	const double relief = Min(1.0, Min(m_mapWidth, m_mapDepth) / config.terrain_fullReliefMapSize);
	const double hillHeight = config.terrain_interiorHillHeight * relief;
	const double scale = Min(m_mapWidth, m_mapDepth) * config.terrain_interiorHillScale;
	std::atomic<int> nextRow{0};
	Array<std::thread> workers;
	const unsigned count = Min(8u, Max(1u, std::thread::hardware_concurrency()));
	for (unsigned i = 0; i < count; ++i)
	{
		workers.emplace_back([&]()
		{
			for (int z = nextRow.fetch_add(1); z <= rows; z = nextRow.fetch_add(1))
			{
				for (int x = 0; x <= columns; ++x)
				{
					float base, amplitude;
					const float wx = static_cast<float>(x * m_macroStepX), wz = static_cast<float>(z * m_macroStepZ);
					computeRawBiomeParams(wx, wz, base, amplitude);
					m_macroTerrain[{x, z}] = {base, amplitude};
					noise[{x, z}] = static_cast<float>(m_perlin.noise2D0_1(wx / scale + 171.0, wz / scale + 513.0));
				}
			}
		});
	}
	for (auto& worker : workers)
	{
		worker.join();
	}
	// 平野になる格子の「丘陵へ変わる閾値」を順位付けする。一律の環状山地は作らない。
	Array<double> thresholds;
	for (int z = 0; z < rows; ++z)
	{
		for (int x = 0; x < columns; ++x)
		{
			const auto value = m_macroTerrain[{x, z}];
			if (value.x < 0 || value.x > config.terrain_mountainBiomeHeight || value.y > config.terrain_hillBiomeRelief)
			{
				continue;
			}
			const double coastalFade = Clamp(value.x / 20.0, 0.0, 1.0);
			const double required = (config.terrain_hillBiomeRelief + .01 - value.y) / config.terrain_mountainDetail;
			thresholds << noise[{x, z}] - Sqrt(required / Max(.001, hillHeight * coastalFade));
		}
	}
	double threshold = config.terrain_interiorHillThreshold;
	// 補間境界の誤差を含めても半分を超えないよう、面積に2ポイントの余裕を持たせる。
	const size_t allowed = static_cast<size_t>(columns * rows * (config.terrain_maximumPlainFraction - .02));
	if (thresholds.size() > allowed)
	{
		thresholds.sort();
		threshold = Min(threshold, thresholds[allowed]);
	}
	for (int z = 0; z <= rows; ++z)
	{
		for (int x = 0; x <= columns; ++x)
		{
			auto& value = m_macroTerrain[{x, z}];
			if (value.x <= 0)
			{
				continue;
			}
			const double hill =
				Square(Max(0.0, noise[{x, z}] - threshold)) * hillHeight * Clamp(value.x / 20.0, 0.0, 1.0);
			value.x += static_cast<float>(hill);
			value.y += static_cast<float>(hill * config.terrain_mountainDetail);
		}
	}
}

void World::computeBiomeParams(float wx, float wz, float& outBase, float& outAmp) const
{
	if (m_macroTerrain.isEmpty() || wx < 0 || wz < 0 || wx > m_mapWidth || wz > m_mapDepth)
	{
		computeRawBiomeParams(wx, wz, outBase, outAmp);
		return;
	}
	const double x = wx / m_macroStepX, z = wz / m_macroStepZ;
	const int col = Min(static_cast<int>(m_macroTerrain.width()) - 2, static_cast<int>(x));
	const int row = Min(static_cast<int>(m_macroTerrain.height()) - 2, static_cast<int>(z));
	const auto a = m_macroTerrain[{col, row}], b = m_macroTerrain[{col + 1, row}];
	const auto c = m_macroTerrain[{col, row + 1}], d = m_macroTerrain[{col + 1, row + 1}];
	outBase = static_cast<float>(Math::Lerp(Math::Lerp(a.x, b.x, x - col), Math::Lerp(c.x, d.x, x - col), z - row));
	outAmp = static_cast<float>(Math::Lerp(Math::Lerp(a.y, b.y, x - col), Math::Lerp(c.y, d.y, x - col), z - row));
}

BiomeType World::getBiome(float wx, float wz) const
{
	float base, amplitude; computeBiomeParams(wx, wz, base, amplitude);
	if (base < 0)
	{
		const Vec2 point{wx-m_mapWidth*.5,wz-m_mapDepth*.5};
		return lakeInfluence(point.dot(m_landAxis),point.dot(Vec2{-m_landAxis.y,m_landAxis.x}),Min(m_mapWidth,m_mapDepth))>0
			? BiomeType::Lake : BiomeType::Ocean;
	}
	if (base > GenerationSettings::get().terrain_mountainRangeBiomeHeight) { return BiomeType::MountainRange; }
	if (base > GenerationSettings::get().terrain_mountainBiomeHeight) { return BiomeType::Mountain; }
	if (amplitude > GenerationSettings::get().terrain_foothillBiomeRelief) { return BiomeType::Foothill; }
	if (amplitude > GenerationSettings::get().terrain_hillBiomeRelief) { return base < GenerationSettings::get().terrain_coastalHillHeight ? BiomeType::CoastalHill : BiomeType::Hill; }
	if (base < GenerationSettings::get().terrain_coastalPlainHeight) { return BiomeType::CoastalPlain; }
	return BiomeType::Basin;
}

float World::computeBaseHeight(float wx, float wz) const
{
	float base, amplitude; computeBiomeParams(wx, wz, base, amplitude);
	if (base < 0) { return base; }
	const double detail = m_perlin.octave2D0_1(wx * GenerationSettings::get().terrain_detailFrequency, wz * GenerationSettings::get().terrain_detailFrequency, GenerationSettings::get().terrain_detailOctaves, GenerationSettings::get().terrain_detailPersistence);
	return static_cast<float>(base + (detail - .5) * amplitude);
}

HeightMapResult World::buildHeightMap(Point chunkCoord) const
{
	// 1 チャンク分の格子点高さをまとめて生成し、最小・最大標高も同時に記録する。
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	Grid<float> hm(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 0.0f);
	float lo =  1e30f;
	float hi = -1e30f;
	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			const float wx = chunkCoord.x * CHUNK_SIZE + col * cellSize;
			const float wz = chunkCoord.y * CHUNK_SIZE + row * cellSize;
			const float h  = computeHeight(wx, wz);
			hm[{ col, row }] = h;
			if (h < lo) lo = h;
			if (h > hi) hi = h;
		}
	}
	return { std::move(hm), lo, hi };
}

void World::generateRivers()
{
	m_rivers.generate(m_mapWidth,m_mapDepth,[this](double x,double z) { return computeBaseHeight(static_cast<float>(x),static_cast<float>(z)); }, m_seed);
}

float World::computeHeight(float wx,float wz) const
{
	return static_cast<float>(m_rivers.carveHeight({wx,wz},computeBaseHeight(wx,wz)));
}

void World::setGridHeight(int x,int z,float height)
{
	for (int cz=Max(0,(z-1)/HEIGHT_CELLS);cz<=Min(WORLD_CHUNKS-1,z/HEIGHT_CELLS);++cz)
		for (int cx=Max(0,(x-1)/HEIGHT_CELLS);cx<=Min(WORLD_CHUNKS-1,x/HEIGHT_CELLS);++cx)
		{
			if (auto* chunk=getChunk({cx,cz})) { chunk->heightMap[{x-cx*HEIGHT_CELLS,z-cz*HEIGHT_CELLS}]=height; chunk->meshDirty=true; }
		}
}
