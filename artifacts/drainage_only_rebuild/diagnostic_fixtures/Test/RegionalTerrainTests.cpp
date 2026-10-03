#include <cmath>
#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "TerrainFoundationMap.hpp"
#include "TerrainFoundationProbe.hpp"
#include "src/world/World.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/railway/RailTimetable.hpp"

namespace
{
	constexpr int kCells=256;
	constexpr double kStep=static_cast<double>(WORLD_SIZE)/kCells;

	/// @brief 実際の高さを測り、中央の適地・外周の海山・平野の起伏と海岸線長を記録する。
	JSON surveyRegion(uint64 seed)
	{
		World world; world.setGenerationParams(seed,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(kCells+1,kCells+1);
		BinaryWriter raw{U"TestResults/terrain_{}.f32"_fmt(seed)};
		for (int z=0;z<=kCells;++z) { for (int x=0;x<=kCells;++x)
		{
			const float height=world.computeHeight(static_cast<float>(x*kStep),static_cast<float>(z*kStep));
			heights[{x,z}]=height; raw.write(height);
		} }
		Grid<uint8> ocean(kCells+1,kCells+1,0);
		Array<Point> pending;
		for (int z=0;z<=kCells;++z) { for (int x=0;x<=kCells;++x)
		{
			if ((x==0 || x==kCells || z==0 || z==kCells) && heights[{x,z}]<0)
			{
				ocean[{x,z}]=1; pending << Point{x,z};
			}
		} }
		for (size_t i=0;i<pending.size();++i)
		{
			for (const Point direction : {Point{-1,0},Point{1,0},Point{0,-1},Point{0,1}})
			{
				const Point next=pending[i]+direction;
				if (next.x<0 || next.y<0 || next.x>kCells || next.y>kCells || ocean[next] || heights[next]>=0) { continue; }
				ocean[next]=1; pending << next;
			}
		}
		int core=0,coreDry=0,coreWater=0,coreMountain=0,edge=0,edgeBarrier=0,lake=0;
		Array<double> relief;
		for (int z=2;z<kCells-1;++z) { for (int x=2;x<kCells-1;++x)
		{
			const double h=heights[{x,z}];
			const double slope=Max(Abs(heights[{x+1,z}]-heights[{x-1,z}]),Abs(heights[{x,z+1}]-heights[{x,z-1}]))/(2*kStep);
			if (x>=64 && x<=192 && z>=64 && z<=192)
			{
				++core; coreDry+=h>3.2 && h<150 && slope<.035; coreWater+=h<0; coreMountain+=h>600;
				lake+=h<0 && !ocean[{x,z}] && world.getBiome(static_cast<float>(x*kStep),static_cast<float>(z*kStep))==BiomeType::Lake;
			}
			if (x<32 || x>224 || z<32 || z>224) { ++edge; edgeBarrier+=h<0 || h>300; }
			if (h>8 && h<150 && slope<.02)
			{
				double low=h,high=h;
				for (const Point offset : {Point{-2,0},Point{2,0},Point{0,-2},Point{0,2}})
				{
					const double sample=heights[Point{x,z}+offset]; low=Min(low,sample); high=Max(high,sample);
				}
				if (low>3.2 && high<180) { relief << high-low; }
			}
		} }
		double shoreLength=0; Vec2 lower{1e30,1e30},upper{-1e30,-1e30};
		for (int z=0;z<kCells;++z) { for (int x=0;x<kCells;++x)
		{
			const Array<Point> corners{{x,z},{x+1,z},{x+1,z+1},{x,z+1}};
			Array<Vec2> crossings;
			for (size_t side=0;side<4;++side)
			{
				const Point a=corners[side],b=corners[(side+1)%4];
				const double first=heights[a],last=heights[b];
				if ((first<0)==(last<0) || !(ocean[a] || ocean[b])) { continue; }
				const double t=first/(first-last);
				const Vec2 point=(Vec2{a.x,a.y}+(Vec2{b.x,b.y}-Vec2{a.x,a.y})*t)*kStep;
				crossings << point;
				lower.x=Min(lower.x,point.x); lower.y=Min(lower.y,point.y); upper.x=Max(upper.x,point.x); upper.y=Max(upper.y,point.y);
			}
			for (size_t i=1;i<crossings.size();i+=2) { shoreLength+=crossings[i-1].distanceFrom(crossings[i]); }
		} }
		relief.sort();
		JSON result; result[U"seed"]=seed;
		result[U"upliftedCells"]=world.terrainEvolution().uplifted;
		result[U"erodedCells"]=world.terrainEvolution().eroded;
		result[U"depositedCells"]=world.terrainEvolution().deposited;
		result[U"upliftMetres"]=world.terrainEvolution().upliftMetres;
		result[U"erosionMetres"]=world.terrainEvolution().erosionMetres;
		result[U"depositionMetres"]=world.terrainEvolution().depositionMetres;
		result[U"coreDry"]=static_cast<double>(coreDry)/core;
		result[U"coreWater"]=static_cast<double>(coreWater)/core; result[U"coreMountain"]=static_cast<double>(coreMountain)/core;
		result[U"edgeBarrier"]=static_cast<double>(edgeBarrier)/edge;
		result[U"plainRelief1024"]=relief.isEmpty() ? 0 : relief[relief.size()/2];
		result[U"shoreLengthKm"]=shoreLength/1000;
		result[U"shoreSinuosity"]=shoreLength/Max(1.0,upper.distanceFrom(lower));
		result[U"inlandLakeKm2"]=lake*kStep*kStep/1000000;
		return result;
	}
	constexpr double kFoundationMinimumDryHeight = 3.2, kFoundationMaximumPlainHeight = 150;
	constexpr double kFoundationBuildableGrade = .035, kFoundationLocalProbe = 8;
	constexpr double kFoundationMountainHeight = 600;
	constexpr double kFoundationMinimumBuildableFraction = .05, kFoundationMinimumConnectedKm2 = 25;
	constexpr double kFoundationBuildableRetention = .8, kFoundationConnectedRetention = .65;
	constexpr double kFoundationMinimumReliefFraction = .01, kFoundationMountainRetention = .95, kFoundationFoothillRetention = .9;
	constexpr double kFoundationMinimumOceanFraction = .01, kFoundationMaximumPlainToSeaMetres = 4096;

	/// @brief A sampled region survey, independent of private valley axes and terrain-generation formulas.
	struct FoundationTerrainSurvey
	{
		Grid<float> heights{ kCells + 1, kCells + 1 };
		Grid<uint8> buildable{ kCells + 1, kCells + 1, 0 };
		Grid<int> component{ kCells + 1, kCells + 1, 0 };
		Grid<uint8> ocean{ kCells + 1, kCells + 1, 0 };
		int buildableCells = 0, largestComponent = 0, largestComponentId = 0;
		int mountainCells = 0, foothillCells = 0, nonfinite = 0;
		JSON measurements;
	};

	/// @brief Local finite differences are 16 m wide; the 256 m lattice measures regional area/connectivity only.
	FoundationTerrainSurvey surveyFoundationTerrain(const World& world)
	{
		FoundationTerrainSurvey result;
		double minimumHeight = Math::Inf, maximumHeight = -Math::Inf, maximumAbsoluteGrade = 0;
		for (int z = 0; z <= kCells; ++z)
		{
			for (int x = 0; x <= kCells; ++x)
			{
				const double wx = x * kStep, wz = z * kStep;
				const double left = Max(0.0, wx - kFoundationLocalProbe), right = Min(static_cast<double>(WORLD_SIZE), wx + kFoundationLocalProbe);
				const double top = Max(0.0, wz - kFoundationLocalProbe), bottom = Min(static_cast<double>(WORLD_SIZE), wz + kFoundationLocalProbe);
				const auto height = [&](double a, double b) { return world.computeHeight(static_cast<float>(a), static_cast<float>(b)); };
				const double ground = height(wx, wz);
				const double leftHeight = height(left, wz), rightHeight = height(right, wz);
				const double topHeight = height(wx, top), bottomHeight = height(wx, bottom);
				result.heights[{x, z}] = static_cast<float>(ground);
				int invalidHeights = 0;
				for (const double sample : {ground, leftHeight, rightHeight, topHeight, bottomHeight})
				{
					invalidHeights += !std::isfinite(sample);
				}
				if (invalidHeights > 0) { result.nonfinite += invalidHeights; continue; }
				const double grade = Max(Abs(rightHeight - leftHeight) / (right - left),
					Abs(bottomHeight - topHeight) / (bottom - top));
				if (!std::isfinite(grade)) { ++result.nonfinite; continue; }
				minimumHeight = Min(minimumHeight, ground); maximumHeight = Max(maximumHeight, ground);
				maximumAbsoluteGrade = Max(maximumAbsoluteGrade, grade);
				const bool buildable = ground > kFoundationMinimumDryHeight && ground < kFoundationMaximumPlainHeight && grade < kFoundationBuildableGrade;
				result.buildable[{x, z}] = buildable ? 1 : 0;
				result.buildableCells += buildable;
				result.mountainCells += ground >= kFoundationMountainHeight;
				result.foothillCells += ground >= kFoundationMaximumPlainHeight && ground < kFoundationMountainHeight;
			}
		}
		constexpr Point kDirections[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
		const auto inside = [](Point point) { return point.x >= 0 && point.y >= 0 && point.x <= kCells && point.y <= kCells; };
		Array<Point> pending;
		int componentId = 0;
		for (int z = 0; z <= kCells; ++z)
		{
			for (int x = 0; x <= kCells; ++x)
			{
				const Point start{x, z};
				if (!result.buildable[start] || result.component[start] != 0) { continue; }
				++componentId; pending.clear(); pending << start; result.component[start] = componentId;
				for (size_t index = 0; index < pending.size(); ++index)
				{
					for (const Point direction : kDirections)
					{
						const Point next = pending[index] + direction;
						if (!inside(next) || !result.buildable[next] || result.component[next] != 0) { continue; }
						// Reject a coarse adjacency if its midpoint is wet/high or its half-span is too steep.
						const Vec2 middle = (Vec2{pending[index].x, pending[index].y} + Vec2{next.x, next.y}) * (kStep * .5);
						const double midpointHeight = world.computeHeight(static_cast<float>(middle.x), static_cast<float>(middle.y));
						if (!std::isfinite(midpointHeight)) { ++result.nonfinite; continue; }
						const double grade = Max(Abs(midpointHeight - result.heights[pending[index]]), Abs(midpointHeight - result.heights[next])) / (kStep * .5);
						if (midpointHeight <= kFoundationMinimumDryHeight || midpointHeight >= kFoundationMaximumPlainHeight || grade >= kFoundationBuildableGrade) { continue; }
						result.component[next] = componentId; pending << next;
					}
				}
				if (static_cast<int>(pending.size()) > result.largestComponent)
				{
					result.largestComponent = static_cast<int>(pending.size()); result.largestComponentId = componentId;
				}
			}
		}
		pending.clear();
		for (int z = 0; z <= kCells; ++z)
		{
			for (int x = 0; x <= kCells; ++x)
			{
				const Point point{x, z};
				if ((x == 0 || z == 0 || x == kCells || z == kCells) && result.heights[point] < 0
					&& world.getBiome(static_cast<float>(x * kStep), static_cast<float>(z * kStep)) == BiomeType::Ocean)
				{
					result.ocean[point] = 1; pending << point;
				}
			}
		}
		for (size_t index = 0; index < pending.size(); ++index)
		{
			for (const Point direction : kDirections)
			{
				const Point next = pending[index] + direction;
				if (!inside(next) || result.ocean[next] || !std::isfinite(result.heights[next]) || result.heights[next] >= 0
					|| world.getBiome(static_cast<float>(next.x * kStep), static_cast<float>(next.y * kStep)) != BiomeType::Ocean) { continue; }
				result.ocean[next] = 1; pending << next;
			}
		}
		const int oceanCells = static_cast<int>(pending.size());
		int openBaySamples = 0;
		constexpr int kBayMinimumInsetCells = 8, kBayMaximumHalfWidthCells = 16;
		for (const Point point : pending)
		{
			if (point.x < kBayMinimumInsetCells || point.y < kBayMinimumInsetCells
				|| point.x > kCells - kBayMinimumInsetCells || point.y > kCells - kBayMinimumInsetCells) { continue; }
			bool bay = false;
			for (const Point direction : {Point{1, 0}, Point{0, 1}})
			{
				bool firstShore = false, secondShore = false;
				for (int distance = 1; distance <= kBayMaximumHalfWidthCells; ++distance)
				{
					const Point first = point + direction * distance, second = point - direction * distance;
					firstShore = firstShore || (inside(first) && result.heights[first] > kFoundationMinimumDryHeight);
					secondShore = secondShore || (inside(second) && result.heights[second] > kFoundationMinimumDryHeight);
				}
				bay = bay || (firstShore && secondShore);
			}
			openBaySamples += bay;
		}
		Grid<int> distance{kCells + 1, kCells + 1, -1};
		pending.clear();
		for (int z = 0; z <= kCells; ++z)
		{
			for (int x = 0; x <= kCells; ++x)
			{
				const Point point{x, z};
				if (result.largestComponentId != 0 && result.component[point] == result.largestComponentId)
				{
					distance[point] = 0; pending << point;
				}
			}
		}
		int closestSeaSteps = (kCells + 1) * 2;
		for (size_t index = 0; index < pending.size(); ++index)
		{
			const Point point = pending[index];
			if (result.ocean[point]) { closestSeaSteps = distance[point]; break; }
			for (const Point direction : kDirections)
			{
				const Point next = point + direction;
				if (!inside(next) || distance[next] >= 0) { continue; }
				distance[next] = distance[point] + 1; pending << next;
			}
		}
		const double sampleCount = Square(static_cast<double>(kCells + 1));
		result.measurements[U"nonfinite"] = result.nonfinite;
		result.measurements[U"buildableFraction"] = result.buildableCells / sampleCount;
		result.measurements[U"largestPlainKm2"] = result.largestComponent * kStep * kStep / 1000000;
		result.measurements[U"largestPlainShare"] = static_cast<double>(result.largestComponent) / Max(1, result.buildableCells);
		result.measurements[U"mountainFraction"] = result.mountainCells / sampleCount;
		result.measurements[U"foothillFraction"] = result.foothillCells / sampleCount;
		result.measurements[U"openOceanFraction"] = oceanCells / sampleCount;
		result.measurements[U"openBaySamples"] = openBaySamples;
		result.measurements[U"largestPlainToSeaManhattanMetres"] = closestSeaSteps * kStep;
		result.measurements[U"minimumHeight"] = minimumHeight; result.measurements[U"maximumHeight"] = maximumHeight;
		result.measurements[U"maximumAbsoluteLocalGrade"] = maximumAbsoluteGrade;
		return result;
	}

	/// @brief Record native relief separately from the grade and cut introduced by river carving.
	JSON surveyFoundationBanks(const World& before, const World& after)
	{
		constexpr size_t kMaximumSections = 256;
		constexpr double kSurveyStep = 16, kOutsideMargin = 32;
		const auto& reaches = after.rivers().reaches;
		const size_t stride = Max(size_t{1}, (reaches.size() + kMaximumSections - 1) / kMaximumSections);
		double maximumOriginalGrade = 0, maximumCarvedGrade = 0, maximumIntroducedGrade = 0, maximumCut = 0;
		int samples = 0, nonfinite = 0;
		JSON worstIntroduced;
		for (size_t id = 0; id < reaches.size(); id += stride)
		{
			const auto& reach = reaches[id];
			const Vec2 a{reach.start.x, reach.start.z}, b{reach.end.x, reach.end.z}, delta = b - a;
			if (delta.lengthSq() < 1e-8) { continue; }
			const Vec2 center = (a + b) * .5, normal = Vec2{-delta.y, delta.x}.normalized();
			const double extent = Max(reach.halfWidth, reach.endHalfWidth) + reach.bankExtent + kOutsideMargin;
			bool havePrevious = false;
			double previousOriginal = 0, previousCarved = 0;
			for (double offset = -extent; offset <= extent; offset += kSurveyStep)
			{
				const Vec2 point = center + normal * offset;
				if (point.x < 0 || point.y < 0 || point.x > WORLD_SIZE || point.y > WORLD_SIZE) { havePrevious = false; continue; }
				const double original = before.computeHeight(static_cast<float>(point.x), static_cast<float>(point.y));
				const double carved = after.computeHeight(static_cast<float>(point.x), static_cast<float>(point.y));
				++samples;
				if (!std::isfinite(original) || !std::isfinite(carved)) { ++nonfinite; havePrevious = false; continue; }
				maximumCut = Max(maximumCut, original - carved);
				if (havePrevious)
				{
					const double originalGrade = Abs(original - previousOriginal) / kSurveyStep;
					const double carvedGrade = Abs(carved - previousCarved) / kSurveyStep;
					const double introducedGrade = Abs((carved - original) - (previousCarved - previousOriginal)) / kSurveyStep;
					maximumOriginalGrade = Max(maximumOriginalGrade, originalGrade); maximumCarvedGrade = Max(maximumCarvedGrade, carvedGrade);
					if (introducedGrade > maximumIntroducedGrade)
					{
						maximumIntroducedGrade = introducedGrade;
						worstIntroduced[U"reach"] = id; worstIntroduced[U"point"] = Array<double>{point.x, point.y};
						worstIntroduced[U"originalGrade"] = originalGrade; worstIntroduced[U"carvedGrade"] = carvedGrade;
						worstIntroduced[U"introducedGrade"] = introducedGrade; worstIntroduced[U"cutMetres"] = original - carved;
					}
				}
				previousOriginal = original; previousCarved = carved; havePrevious = true;
			}
		}
		JSON result;
		result[U"samples"] = samples; result[U"nonfinite"] = nonfinite;
		result[U"maximumOriginalGrade"] = maximumOriginalGrade; result[U"maximumCarvedGrade"] = maximumCarvedGrade;
		result[U"maximumIntroducedGrade"] = maximumIntroducedGrade; result[U"maximumCutMetres"] = maximumCut;
		result[U"worstIntroducedGrade"] = worstIntroduced;
		return result;
	}

}

void registerRegionalTerrainTests(TestRunner& runner)
{
	runner.add(U"Terrain.FoundationMap.Seed42", TerrainFoundationMap::run);
	runner.add(U"Terrain.FoundationProbe.Seed42", TerrainFoundationProbe::run);
	runner.add(U"Terrain.UnreachableStationKeepsUsableRailway",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=0;z<16;++z) { for (int x=0;x<16;++x)
		{
			Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20);
			for (int row=0;row<=HEIGHT_CELLS;++row) { for (int col=0;col<=HEIGHT_CELLS;++col)
			{
				const Vec2 point{x*CHUNK_SIZE+col*16.0,z*CHUNK_SIZE+row*16.0};
				heights[{col,row}]=static_cast<float>(20+1800*Max(0.0,1-point.distanceFrom(Vec2{4000,4000})/300));
			} }
			world.installChunkDirect({x,z},HeightMapResult{heights,20,1820});
		} }
		Array<MapGenerator::Settlement> towns;
		for (int index=0;index<3;++index)
		{
			MapGenerator::Settlement town;
			town.center={index==0 ? 4000.0 : 6000.0+index*1000,4000};
			town.gridAxisX={1,0}; town.gridAxisZ={0,1}; town.plan.station=Vec2{0,0};
			town.name=U"駅候補{}"_fmt(index); towns << town;
		}
		TrainNetwork network;
		RailwayAlignment::generate(network,world,towns);
		Array<int> stations;
		for (const auto& node : network.nodes()) { if (node.type==TrackNodeType::Station) { stations << node.id; } }
		context.expectEqual(stations.size(),size_t{2},U"An unreachable first candidate does not prevent the two lowland stations from being built");
		context.expectEqual(network.schedules().size(),size_t{1},U"The feasible railway receives a default timetable");
		if (stations.size()==2) { context.expect(!network.findRoute(stations[0],stations[1]).isEmpty(),U"Built stations are connected, without an isolated phantom station on the mountain"); }
		for (const auto& schedule : network.schedules()) { context.expect(RailTimetable::validate(network,schedule).isEmpty(),U"Generated partial railways retain valid editable timetables"); }
	});
	for (const uint64 seed : {uint64{7}, uint64{42}, uint64{130}, uint64{2026}})
	{
		runner.add(U"Terrain.Foundation.PostRiverUsableRegion.Seed{}"_fmt(seed), [seed](TestContext& context)
		{
			World original, carved;
			original.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			carved.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE); carved.generateRivers();
			const auto before = surveyFoundationTerrain(original), after = surveyFoundationTerrain(carved);
			int retainedMountains = 0, retainedFoothills = 0, retainedBuildable = 0;
			for (int z = 0; z <= kCells; ++z)
			{
				for (int x = 0; x <= kCells; ++x)
				{
					const Point point{x, z};
					retainedBuildable += before.buildable[point] && after.buildable[point];
					retainedMountains += before.heights[point] >= kFoundationMountainHeight && after.heights[point] >= kFoundationMountainHeight;
					retainedFoothills += before.heights[point] >= kFoundationMaximumPlainHeight && before.heights[point] < kFoundationMountainHeight
						&& after.heights[point] >= kFoundationMaximumPlainHeight && after.heights[point] < kFoundationMountainHeight;
				}
			}
			const double buildableRetention = static_cast<double>(retainedBuildable) / Max(1, before.buildableCells);
			const double connectedRetention = static_cast<double>(after.largestComponent) / Max(1, before.largestComponent);
			const double mountainRetention = static_cast<double>(retainedMountains) / Max(1, before.mountainCells);
			const double foothillRetention = static_cast<double>(retainedFoothills) / Max(1, before.foothillCells);
			const JSON banks = surveyFoundationBanks(original, carved);
			JSON report; report[U"seed"] = seed; report[U"beforeRivers"] = before.measurements; report[U"afterRivers"] = after.measurements;
			report[U"buildableRetention"] = buildableRetention; report[U"largestConnectedPlainRetention"] = connectedRetention;
			report[U"mountainRetention"] = mountainRetention; report[U"foothillRetention"] = foothillRetention; report[U"bankSections"] = banks;
			report.save(U"TestResults/terrain_foundation_seed_{}.json"_fmt(seed));
			context.expect(!carved.rivers().reaches.isEmpty(), U"Post-river terrain gates require actual generated channels");
			context.expectEqual(before.nonfinite + after.nonfinite + banks[U"nonfinite"].get<int>(), 0, U"Both terrain fields and bank sections remain finite");
			context.expect(after.measurements[U"buildableFraction"].get<double>() >= kFoundationMinimumBuildableFraction, U"At least 5% of the region remains low, locally buildable terrain");
			context.expect(after.measurements[U"largestPlainKm2"].get<double>() >= kFoundationMinimumConnectedKm2, U"A sampled connected plain of at least 25 square kilometres remains usable");
			context.expect(buildableRetention >= kFoundationBuildableRetention && connectedRetention >= kFoundationConnectedRetention, U"Rivers retain 80% of buildable samples and 65% of the largest connected plain");
			context.expect(after.measurements[U"mountainFraction"].get<double>() >= kFoundationMinimumReliefFraction && after.measurements[U"foothillFraction"].get<double>() >= kFoundationMinimumReliefFraction,
				U"Mountains above 600 m and foothills from 150 to 600 m each retain a meaningful regional area");
			context.expect(mountainRetention >= kFoundationMountainRetention && foothillRetention >= kFoundationFoothillRetention, U"Rivers preserve 95% of mountain samples and 90% of foothill samples");
			context.expect(after.measurements[U"openOceanFraction"].get<double>() >= kFoundationMinimumOceanFraction && after.measurements[U"openBaySamples"].get<int>() > 0,
				U"A boundary-connected sea includes an inset bay with dry opposing shores");
			context.expect(after.measurements[U"largestPlainToSeaManhattanMetres"].get<double>() <= kFoundationMaximumPlainToSeaMetres,
				U"The largest connected plain lies within four kilometres of the open sea or its bay");
			context.expect(banks[U"samples"].get<int>() > 0, U"Narrow bank sections distinguish natural grade from the introduced cut");
		});
	}

	runner.add(U"Terrain.RegionalComposition",[](TestContext& context)
	{
		const Array<uint64> seeds{7,42,130,2026,0,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16};
		JSON report; double central=0,periphery=0,roughness=0,coast=0;
		int centralPlains=0,centralMountains=0,centralBays=0,inlandLakes=0;
		for (size_t i=0;i<seeds.size();++i)
		{
			const auto result=surveyRegion(seeds[i]); report[U"seeds"][i]=result;
			context.expect(result[U"upliftedCells"].get<int>()>0 && result[U"erodedCells"].get<int>()>0
				&& result[U"depositedCells"].get<int>()>0,U"Terrain receives uplift, stream erosion and downstream deposition");
			central+=result[U"coreDry"].get<double>(); periphery+=result[U"edgeBarrier"].get<double>();
			roughness+=result[U"plainRelief1024"].get<double>(); coast+=result[U"shoreSinuosity"].get<double>();
			centralPlains+=result[U"coreDry"].get<double>()>.5;
			centralMountains+=result[U"coreMountain"].get<double>()>.08;
			centralBays+=result[U"coreWater"].get<double>()>.08;
			inlandLakes+=result[U"inlandLakeKm2"].get<double>()>1;
		}
		const double count=static_cast<double>(seeds.size());
		report[U"meanCoreDry"]=central/count; report[U"meanEdgeBarrier"]=periphery/count;
		report[U"meanPlainRelief1024"]=roughness/count; report[U"meanShoreSinuosity"]=coast/count;
		report[U"centralPlains"]=centralPlains; report[U"centralMountains"]=centralMountains;
		report[U"centralBays"]=centralBays; report[U"inlandLakes"]=inlandLakes;
		report.save(U"TestResults/terrain_composition.json");
		context.expect(central / count > .35 && centralPlains >= 3,
			U"Interior hills leave developable valleys without requiring one broad central plain");
		context.expect(periphery/count>.55,U"Map edges tend to be mountains or sea without a compulsory border wall");
		context.expect(roughness/count>5 && roughness/count<24,U"Plains contain gentle kilometre-scale undulations while remaining buildable");
		context.expect(coast/count>1.6,U"Ocean shores have real inlets and capes beyond a nearly straight boundary");
		context.expect(centralMountains>0 && centralBays>0 && inlandLakes>0,U"Central mountains, large bays and inland lakes remain possible");
	});
}
