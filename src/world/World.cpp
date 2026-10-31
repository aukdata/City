#include <filesystem>
#include <fstream>
#include "../gen/GenerationSettings.hpp"
#include "World.hpp"
#include <cmath>
#include "../sim/ParallelJobs.hpp"
#include <queue>
#include <map>

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
	m_sourceWater.reset(); m_waterBacklog = m_lastWaterLog = 0.0;
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
	double plain=kPlainFloor+inland*kInlandGradient+reliefScale*(kBroadPlainRelief*broad+kLocalPlainRelief*local);
	// 盆地の河岸段丘: 平らな段丘面と短い段丘崖を交互に作る。上田・松本平のように田が並ぶ平坦面を広く残す。
	const auto& terrace=GenerationSettings::get();
	if (terrace.terrain_terraceHeight>0 && terrace.terrain_terraceStrength>0)
	{
		const double level=plain/terrace.terrain_terraceHeight,tread=Math::Floor(level),fraction=level-tread;
		const double riser=Clamp((fraction-(1-terrace.terrain_terraceRiser))/terrace.terrain_terraceRiser,0.0,1.0);
		const double stepped=(tread+riser*riser*(3-2*riser))*terrace.terrain_terraceHeight;
		plain=Math::Lerp(plain,stepped,terrace.terrain_terraceStrength*(1-transition(mountain,terrace.terrain_terraceMountainFade)));
	}
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
	const unsigned concurrency = Min(8u, Max(1u, std::thread::hardware_concurrency()));
	ParallelJobs::run(rows + 1, [&](int z)
	{
		for (int x = 0; x <= columns; ++x)
		{
			float base, amplitude;
			const float wx = static_cast<float>(x * m_macroStepX), wz = static_cast<float>(z * m_macroStepZ);
			computeRawBiomeParams(wx, wz, base, amplitude);
			m_macroTerrain[{x, z}] = {base, amplitude};
			noise[{x, z}] = static_cast<float>(m_perlin.noise2D0_1(wx / scale + 171.0, wz / scale + 513.0));
		}
	}, concurrency);
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
	evolveMacroTerrain();
}

/// @brief Route rainfall to the sea, incise steep channels and lay transported sediment on lower gradients.
void World::evolveMacroTerrain()
{
	const int columns=static_cast<int>(m_macroTerrain.width());
	const int rows=static_cast<int>(m_macroTerrain.height());
	const int count=columns*rows;
	const auto& config=GenerationSettings::get();
	m_terrainEvolution={};
	Array<double> ground(count),filled(count),sediment(count,0),change(count,0);
	Array<int> parent(count,-1),flow(count,1),order,coast;
	Array<bool> ocean(count,false),visited(count,false);
	for (int id=0; id<count; ++id)
	{
		const int x=id%columns,z=id/columns;
		ground[id]=m_macroTerrain[{x,z}].x;
		if ((x==0 || z==0 || x==columns-1 || z==rows-1) && ground[id]<=0)
		{ ocean[id]=true; coast << id; }
	}
	for (size_t index=0; index<coast.size(); ++index)
	{
		const int id=coast[index],x=id%columns,z=id/columns;
		for (const Point offset : {Point{-1,0},Point{1,0},Point{0,-1},Point{0,1}})
		{
			const int nx=x+offset.x,nz=z+offset.y;
			if (nx<0 || nx>=columns || nz<0 || nz>=rows) { continue; }
			const int next=nz*columns+nx;
			if (!ocean[next] && ground[next]<=0) { ocean[next]=true; coast << next; }
		}
	}
	m_macroOcean=Grid<uint8>(columns,rows,0);
	for (int id=0; id<count; ++id) { m_macroOcean[{id%columns,id/columns}]=ocean[id] ? 1 : 0; }
	using Entry=std::pair<double,int>;
	std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
	for (int id=0; id<count; ++id)
	{
		const int x=id%columns,z=id/columns;
		const bool boundary=x==0 || z==0 || x==columns-1 || z==rows-1;
		if (ocean[id] || (coast.isEmpty() && boundary))
		{ visited[id]=true; filled[id]=Max(0.0,ground[id]); pending.emplace(filled[id],id); }
	}
	order.reserve(count);
	const double spacing=Min(m_macroStepX,m_macroStepZ);
	while (!pending.empty())
	{
		const auto [level,id]=pending.top(); pending.pop();
		order << id;
		const int x=id%columns,z=id/columns;
		for (int dz=-1; dz<=1; ++dz) for (int dx=-1; dx<=1; ++dx)
		{
			const int nx=x+dx,nz=z+dz;
			if ((dx==0 && dz==0) || nx<0 || nx>=columns || nz<0 || nz>=rows) { continue; }
			const int next=nz*columns+nx;
			if (visited[next]) { continue; }
			visited[next]=true; parent[next]=id;
			filled[next]=Max(ground[next],level+spacing*.0001);
			pending.emplace(filled[next],next);
		}
	}
	for (auto it=order.rbegin(); it!=order.rend(); ++it)
	{
		const int id=*it,next=parent[id];
		if (next<0 || ocean[id]) { continue; }
		flow[next]+=flow[id];
		const int dx=id%columns-next%columns,dz=id/columns-next/columns;
		const double run=std::sqrt(Square(dx*m_macroStepX)+Square(dz*m_macroStepZ));
		const double grade=Max(.0001,(filled[id]-filled[next])/run);
		const double capacity=1.3*std::sqrt(static_cast<double>(flow[id]))*std::sqrt(grade);
		const double eroded=Min(6.0,Max(0.0,capacity-sediment[id])*config.terrain_erosionRate);
		const double deposited=Min(3.0,Max(0.0,sediment[id]-capacity)*config.terrain_depositionRate);
		sediment[next]+=sediment[id]+eroded-deposited;
		const double uplifted=Min(3.0,Max(0.0,ground[id])*config.terrain_upliftRate);
		change[id]=uplifted-eroded+deposited;
		m_terrainEvolution.uplifted+=uplifted>.01;
		m_terrainEvolution.eroded+=eroded>.01;
		m_terrainEvolution.deposited+=deposited>.01;
		m_terrainEvolution.upliftMetres+=uplifted;
		m_terrainEvolution.erosionMetres+=eroded;
		m_terrainEvolution.depositionMetres+=deposited;
	}
	for (int z=0; z<rows; ++z) for (int x=0; x<columns; ++x)
	{
		const int id=z*columns+x;
		if (ground[id]<=0) { continue; }
		double reshaping=change[id]*.5;
		for (const Point offset : {Point{-1,0},Point{1,0},Point{0,-1},Point{0,1}})
		{
			const int nx=x+offset.x,nz=z+offset.y;
			if (nx>=0 && nx<columns && nz>=0 && nz<rows) { reshaping+=change[nz*columns+nx]*.125; }
		}
		m_macroTerrain[{x,z}].x+=static_cast<float>(reshaping);
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
		if (!m_macroOcean.isEmpty() && wx>=0 && wz>=0 && wx<=m_mapWidth && wz<=m_mapDepth)
		{
			const int x=Clamp(static_cast<int>(Round(wx/m_macroStepX)),0,static_cast<int>(m_macroOcean.width())-1);
			const int z=Clamp(static_cast<int>(Round(wz/m_macroStepZ)),0,static_cast<int>(m_macroOcean.height())-1);
			return m_macroOcean[{x,z}] ? BiomeType::Ocean : BiomeType::Lake;
		}
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
	const auto& config = GenerationSettings::get();
	double detail = m_perlin.octave2D0_1(wx * config.terrain_detailFrequency, wz * config.terrain_detailFrequency, config.terrain_detailOctaves, config.terrain_detailPersistence) - .5;
	// 山地は尾根が鋭く谷が V 字に刻まれた日本の山容にする。前の層の値で次の層を重み付けし、尾根筋に細部を集める。
	const double rugged = Clamp((amplitude - config.terrain_foothillBiomeRelief) / Max(1.0, config.terrain_ruggedRelief - config.terrain_foothillBiomeRelief), 0.0, 1.0);
	if (rugged > 0 && config.terrain_ridgeBlend > 0)
	{
		double ridged = 0, weight = 1, amplitudeSum = 0, gain = 1, frequency = config.terrain_ridgeFrequency;
		for (int octave = 0; octave < config.terrain_ridgeOctaves; ++octave)
		{
			const double signal = Square(1 - Abs(m_perlin.noise2D(wx * frequency + 37.0 * octave, wz * frequency - 91.0 * octave))) * weight;
			ridged += signal * gain; amplitudeSum += gain;
			weight = Clamp(signal * 1.6, 0.0, 1.0); gain *= .5; frequency *= 2.03;
		}
		ridged /= amplitudeSum;
		// 谷底は少し平らに残し、山あいの集落と田が入る余地を作る。
		const double floor = config.terrain_valleyFloor;
		ridged = ridged < floor ? floor - (floor - ridged) * .25 : ridged;
		detail = Math::Lerp(detail, ridged - .35, rugged * rugged * (3 - 2 * rugged) * config.terrain_ridgeBlend);
	}
	return static_cast<float>(base + detail * amplitude);
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
	m_rivers.generate(m_mapWidth,m_mapDepth,
		[this](double x,double z) { return computeBaseHeight(static_cast<float>(x),static_cast<float>(z)); },
		[this](double x,double z) { return getBiome(static_cast<float>(x),static_cast<float>(z))==BiomeType::Ocean; });
}

void World::generateSourceWater(bool riversEnabled,bool routedRivers)
{
	const Stopwatch timer{StartImmediately::Yes};
	m_rivers = RiverNetwork{};
	const auto height = [this](double x,double z) { return computeBaseHeight(static_cast<float>(x),static_cast<float>(z)); };
	const auto marine = [this](double x,double z) { return getBiome(static_cast<float>(x),static_cast<float>(z)) == BiomeType::Ocean; };
	if (riversEnabled) { m_rivers.generate(m_mapWidth,m_mapDepth,height,marine,!routedRivers,routedRivers); }
	std::vector<SourceWater::Source> sources;
	for (const auto& source : m_rivers.waterSources) { sources.push_back({source.position.x,source.position.y,source.rate}); }
	const auto bed=[this](double x,double z) { return (computeHeight(static_cast<float>(x+8),static_cast<float>(z-8))
		+computeHeight(static_cast<float>(x-8),static_cast<float>(z+8)))*.5; };
	m_sourceWater = std::make_unique<SourceWater>(m_mapWidth,m_mapDepth,bed,marine,sources);
	std::vector<SourceWater::Channel> channels;channels.reserve(m_rivers.reaches.size());
	std::map<std::pair<double,double>,int> downstream;
	for (std::size_t i=0;i<m_rivers.reaches.size();++i) { const auto& reach=m_rivers.reaches[i];downstream[{reach.start.x,reach.start.z}]=static_cast<int>(i); }
	for (const auto& reach:m_rivers.reaches)
	{
		const auto found=downstream.find({reach.end.x,reach.end.z});
		const int next=found!=downstream.end() ? found->second : marine(reach.end.x,reach.end.z) ? -2 : -1;
		channels.push_back({reach.start.x,reach.start.z,reach.end.x,reach.end.z,reach.start.y,reach.end.y,
			reach.halfWidth*2,reach.endHalfWidth*2,Max(.5,GenerationSettings::get().rivers_bedDepth+reach.incision),next,0});
	}
	std::size_t mapped=0;
	for (const auto& source:m_rivers.waterSources)
	{
		const auto found=downstream.find({source.position.x,source.position.y});
		const auto nearest=m_rivers.nearest(source.position);
		const int index=found!=downstream.end() ? found->second : nearest.reach;
		if (index>=0) { channels[static_cast<std::size_t>(index)].inlet+=source.rate;++mapped; }
	}
	if (!channels.empty()) { m_sourceWater->configureChannels(std::move(channels)); }
	DBG_LOG(U"[RiverWater] reaches={} mappedSources={}/{} initialStored={}m3"_fmt(m_rivers.reaches.size(),mapped,sources.size(),m_sourceWater->ownedVolume()));
	m_waterBacklog = m_lastWaterLog = 0.0;
	DBG_LOG(U"[SourceWater] initialized seed={} grid={}x{} spacing={}m sources={} rejected={} seaCells={} seaLevel=0 sinkOnly=1 setupMs={:.2f}"_fmt(
		m_seed,m_sourceWater->grid().width,m_sourceWater->grid().height,m_sourceWater->grid().cellSize,
		m_sourceWater->sourceCount(),m_sourceWater->rejectedSources(),m_sourceWater->oceanCells(),timer.msF()));
}

bool World::saveSourceWater(FilePathView path) const
{
	if (!m_sourceWater) { return false; }
	std::ofstream stream{std::filesystem::path{String{path}.toWstr()},std::ios::binary};
	if (!stream || !m_sourceWater->write(stream)) { return false; }
	stream.flush(); return static_cast<bool>(stream);
}

bool World::verifySourceWater(FilePathView path) const
{
	if (!m_sourceWater) { return false; }
	std::ifstream stream{std::filesystem::path{String{path}.toWstr()},std::ios::binary};
	return stream && m_sourceWater->read(stream,false);
}

bool World::loadSourceWater(FilePathView path)
{
	if (!m_sourceWater) { return false; }
	std::ifstream stream{std::filesystem::path{String{path}.toWstr()},std::ios::binary};
	const bool valid=stream && m_sourceWater->read(stream);
	DBG_LOG(U"[SourceWater] restore path={} valid={} t={}s stored={}m3 seaIn={}m3 balance={}m3"_fmt(
		path,valid,m_sourceWater->elapsed(),m_sourceWater->ownedVolume(),m_sourceWater->inflow(),m_sourceWater->balanceError()));
	return valid;
}

float World::waterBedHeight(double x,double z) const
{
	const Point coord{static_cast<int>(Floor(x/CHUNK_SIZE)),static_cast<int>(Floor(z/CHUNK_SIZE))};
	if (getChunk(coord)) { return sampleHeight(static_cast<float>(x),static_cast<float>(z)); }
	return computeHeight(static_cast<float>(x),static_cast<float>(z));
}

double World::waterSurfaceHeight(double x,double z) const
{
	if (!m_sourceWater) { return m_rivers.waterLevel({x,z}); }
	const auto water = m_sourceWater->sample(x,z,waterBedHeight(x,z));
	return water.depth > 0.003 || water.ocean ? water.head : 0.0;
}

double World::waterPlanningHeight(double x,double z) const
{
	return Max(waterSurfaceHeight(x,z),m_rivers.waterLevel({x,z}));
}

void World::prepareSourceWaterForNewGame()
{
	if (!m_sourceWater || m_sourceWater->channels().empty() || m_sourceWater->sourceCount()==0 || m_sourceWater->elapsed()!=0) { return; }
	const Stopwatch timer{StartImmediately::Yes};
	// Real point-source supply creates the initial river; no water is painted onto dry land.
	constexpr int kPreparationSteps=3600;
	for (int step=0;step<kPreparationSteps;++step) { m_sourceWater->advance(6.0); }
	m_lastWaterLog=m_sourceWater->elapsed();
	DBG_LOG(U"[SourceWater] preparedNewGame t={}s steps={} sources={} injected={}m3 stored={}m3 seaOut={}m3 seaIn={}m3 balance={}m3 wallMs={:.2f}"_fmt(
		m_sourceWater->elapsed(),kPreparationSteps,m_sourceWater->sourceCount(),m_sourceWater->injected(),m_sourceWater->ownedVolume(),
		m_sourceWater->outflow(),m_sourceWater->inflow(),m_sourceWater->balanceError(),timer.msF()));
}

void World::advanceSourceWater(double gameSeconds)
{
	if (!m_sourceWater || gameSeconds <= 0.0) { return; }
	m_waterBacklog += gameSeconds*60.0;
	constexpr double kStep = 6.0;
	if (m_waterBacklog < kStep) { return; }
	const Stopwatch timer{StartImmediately::Yes};
	m_sourceWater->advance(kStep); m_waterBacklog -= kStep;
	if (m_sourceWater->elapsed()-m_lastWaterLog >= 600.0)
	{
		m_lastWaterLog = m_sourceWater->elapsed();
		DBG_LOG(U"[SourceWater] t={:.1f}s stepMs={:.3f} backlog={:.2f}s sources={} injected={:.6f}m3 stored={:.6f}m3 seaOut={:.6f}m3 seaIn={:.6f}m3 balance={:.9g}m3"_fmt(
			m_sourceWater->elapsed(),timer.msF(),m_waterBacklog,m_sourceWater->sourceCount(),m_sourceWater->injected(),
			m_sourceWater->ownedVolume(),m_sourceWater->outflow(),m_sourceWater->inflow(),m_sourceWater->balanceError()));
	}
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
			if (auto* chunk=getChunk({cx,cz})) { chunk->heightMap[{x-cx*HEIGHT_CELLS,z-cz*HEIGHT_CELLS}]=height; chunk->markMeshDirty(); }
		}
	if (m_sourceWater)
	{
		const double spacing = m_sourceWater->grid().cellSize;
		const int cx = static_cast<int>(x*16.0/spacing), cz = static_cast<int>(z*16.0/spacing);
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				const int bx = cx+dx, bz = cz+dz;
				if (bx >= 0 && bz >= 0 && bx < m_sourceWater->grid().width && bz < m_sourceWater->grid().height)
				{
					m_sourceWater->setBed(bx,bz,waterBedHeight((bx+0.5)*spacing,(bz+0.5)*spacing));
				}
			}
		}
	}
}
