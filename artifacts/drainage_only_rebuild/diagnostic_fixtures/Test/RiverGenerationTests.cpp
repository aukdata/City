#include <cmath>
#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/debug/DebugLog.hpp"
#include "src/gen/RiverNetwork.hpp"
#include "src/world/World.hpp"
#include <chrono>

namespace
{
	/// @brief Close the per-seed diagnostics file on normal exit or exception during generation.
	struct RiverDebugLogScope
	{
		~RiverDebugLogScope() { DebugLog::shutdown(); }
	};

	/// @brief 生成された流路と水平断面の交点を測り、平野と峡谷で蛇行量を比較する。
	JSON surveyTrunk(const RiverNetwork& river, const std::function<double(double)>& valley)
	{
		Array<Vec2> points;
		double squaredOffset = 0, maximumOffset = 0, length = 0;
		for (double z = 6000; z <= 14000; z += 40)
		{
			Optional<Vec2> nearest;
			for (const auto& reach : river.reaches)
			{
				if (Abs(reach.end.z - reach.start.z) < 1e-6) { continue; }
				const double t = (z - reach.start.z) / (reach.end.z - reach.start.z);
				if (t < 0 || t > 1) { continue; }
				const Vec2 point{Math::Lerp(reach.start.x, reach.end.x, t), z};
				if (!nearest || Abs(point.x - valley(z)) < Abs(nearest->x - valley(z))) { nearest = point; }
			}
			if (!nearest || Abs(nearest->x - valley(z)) > 800) { continue; }
			const double offset = Abs(nearest->x - valley(z));
			squaredOffset += offset * offset; maximumOffset = Max(maximumOffset, offset);
			if (!points.isEmpty()) { length += points.back().distanceFrom(*nearest); }
			points << *nearest;
		}
		JSON result;
		result[U"samples"] = points.size();
		result[U"rmsOffset"] = std::sqrt(squaredOffset / Max(size_t{1}, points.size()));
		result[U"maximumOffset"] = maximumOffset;
		result[U"sinuosity"] = points.size() < 2 ? 0 : length / points.front().distanceFrom(points.back());
		return result;
	}

	/// @brief 8方位に張り付いた長さ、最大刻み、逆流を完成流路で測定する。
	JSON surveyNetwork(const RiverNetwork& river)
	{
		double length = 0, gridLength = 0, maximumSegment = 0;
		int uphill = 0, crossings = 0;
		HashTable<Point, Array<size_t>> index;
		HashSet<uint64> crossingPairs;
		JSON examples; int exampleCount=0;
		const auto cross = [](Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; };
		for (size_t id = 0; id < river.reaches.size(); ++id)
		{
			const auto& current = river.reaches[id];
			const Vec2 a{current.start.x, current.start.z}, b{current.end.x, current.end.z}, delta = b - a;
			for (int z = static_cast<int>(Floor(Min(a.y, b.y) / 64)); z <= static_cast<int>(Floor(Max(a.y, b.y) / 64)); ++z)
			{
				for (int x = static_cast<int>(Floor(Min(a.x, b.x) / 64)); x <= static_cast<int>(Floor(Max(a.x, b.x) / 64)); ++x)
				{
					auto& nearby = index[Point{x, z}];
					for (const size_t otherId : nearby)
					{
						const auto& other = river.reaches[otherId];
						const Vec2 c{other.start.x, other.start.z}, d{other.end.x, other.end.z};
						if (Min({a.distanceFromSq(c), a.distanceFromSq(d), b.distanceFromSq(c), b.distanceFromSq(d)}) < 1e-6) { continue; }
						const double denominator = cross(delta, d - c);
						if (Abs(denominator) < 1e-9) { continue; }
						const double t = cross(c - a, d - c) / denominator, u = cross(c - a, delta) / denominator;
						if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
							{
								crossingPairs.insert((static_cast<uint64>(id) << 32) | otherId);
								if (exampleCount<3) { examples[Format(exampleCount++)]=Array<double>{a.x,a.y,b.x,b.y,c.x,c.y,d.x,d.y,t,u}; }
							}
					}
					nearby << id;
				}
			}
		}
		crossings = static_cast<int>(crossingPairs.size());
		for (const auto& reach : river.reaches)
		{
			const Vec2 delta{reach.end.x - reach.start.x, reach.end.z - reach.start.z};
			const double run = delta.length();
			const double angle = Atan2(delta.y, delta.x);
			const double residual = Abs(angle - Round(angle / (Math::Pi / 4)) * (Math::Pi / 4));
			length += run; maximumSegment = Max(maximumSegment, run);
			if (residual < 1.5_deg) { gridLength += run; }
			uphill += reach.end.y > reach.start.y + 1e-7;
		}
		JSON result;
		result[U"crossingExamples"] = examples;
		result[U"reaches"] = river.reaches.size(); result[U"lengthKm"] = length / 1000;
		result[U"gridAlignedFraction"] = gridLength / Max(1.0, length);
		result[U"maximumSegment"] = maximumSegment; result[U"uphill"] = uphill; result[U"unconnectedCrossings"] = crossings;
		return result;
	}
	/// @brief Measure the emitted graph independently, keeping distinct water levels at identical plan coordinates.
	JSON surveyFoundationGraph(const RiverNetwork& rivers, double width, double depth,
		const std::function<bool(double, double)>& marine)
	{
		struct SurveyNode
		{
			Vec2 point;
			double minimumWater, maximumWater;
			int incoming = 0;
			Array<size_t> downstream;
		};
		constexpr double kCoordinateTolerance = 1e-7, kWaterTolerance = 1e-7;
		Array<SurveyNode> nodes;
		HashTable<Vec2, size_t> nodeIds;
		const auto nodeId = [&](const Vec3& point)
		{
			// Canonicalize signed zero before hashing, without merging nearby channels.
			const Vec2 plan{point.x == 0 ? 0 : point.x, point.z == 0 ? 0 : point.z};
			const auto found = nodeIds.find(plan);
			if (found != nodeIds.end())
			{
				auto& node = nodes[found->second];
				node.minimumWater = Min(node.minimumWater, point.y);
				node.maximumWater = Max(node.maximumWater, point.y);
				return found->second;
			}
			const size_t id = nodes.size();
			nodeIds.emplace(plan, id);
			nodes << SurveyNode{plan, point.y, point.y, 0, {}};
			return id;
		};
		int nonfinite = 0, outside = 0, uphill = 0;
		for (const auto& reach : rivers.reaches)
		{
			const bool finite = std::isfinite(reach.start.x) && std::isfinite(reach.start.y) && std::isfinite(reach.start.z)
				&& std::isfinite(reach.end.x) && std::isfinite(reach.end.y) && std::isfinite(reach.end.z);
			if (!finite) { ++nonfinite; continue; }
			for (const Vec3 point : {reach.start, reach.end})
			{
				outside += point.x < -kCoordinateTolerance || point.z < -kCoordinateTolerance
					|| point.x > width + kCoordinateTolerance || point.z > depth + kCoordinateTolerance;
			}
			uphill += reach.end.y > reach.start.y + kWaterTolerance;
			const size_t start = nodeId(reach.start), end = nodeId(reach.end);
			nodes[start].downstream << end;
			++nodes[end].incoming;
		}
		Array<int> incoming;
		Array<size_t> queue;
		int confluences = 0, splitOutflows = 0, levelMismatches = 0;
		int oceanMouths = 0, nonzeroMouths = 0, boundaryOutlets = 0, inlandTerminals = 0;
		double maximumSharedWaterGap = 0;
		JSON terminalExamples, sharedWaterExamples;
		for (size_t id = 0; id < nodes.size(); ++id)
		{
			const auto& node = nodes[id];
			incoming << node.incoming;
			if (node.incoming == 0) { queue << id; }
			confluences += node.incoming > 1;
			splitOutflows += node.downstream.size() > 1;
			const double waterGap = node.maximumWater - node.minimumWater;
			maximumSharedWaterGap = Max(maximumSharedWaterGap, waterGap);
			if (waterGap > kWaterTolerance)
			{
				if (levelMismatches < 5)
				{
					sharedWaterExamples[Format(levelMismatches)] = Array<double>{node.point.x, node.point.y, node.minimumWater, node.maximumWater};
				}
				++levelMismatches;
			}
			if (!node.downstream.isEmpty()) { continue; }
			const bool boundary = Abs(node.point.x) <= kCoordinateTolerance || Abs(node.point.y) <= kCoordinateTolerance
				|| Abs(node.point.x - width) <= kCoordinateTolerance || Abs(node.point.y - depth) <= kCoordinateTolerance;
			if (marine(node.point.x, node.point.y))
			{
				++oceanMouths;
				nonzeroMouths += Abs(node.minimumWater) > kWaterTolerance || Abs(node.maximumWater) > kWaterTolerance;
			}
			else if (boundary) { ++boundaryOutlets; }
			else
			{
				if (inlandTerminals < 5) { terminalExamples[Format(inlandTerminals)] = Array<double>{node.point.x, node.point.y, node.minimumWater}; }
				++inlandTerminals;
			}
		}
		for (size_t index = 0; index < queue.size(); ++index)
		{
			for (const size_t next : nodes[queue[index]].downstream)
			{
				if (--incoming[next] == 0) { queue << next; }
			}
		}
		JSON result;
		result[U"reaches"] = rivers.reaches.size(); result[U"nodes"] = nodes.size();
		result[U"nonfinite"] = nonfinite; result[U"outOfBounds"] = outside; result[U"uphill"] = uphill;
		result[U"confluences"] = confluences; result[U"splitOutflows"] = splitOutflows;
		result[U"cyclicOrBlockedNodes"] = nodes.size() - queue.size();
		result[U"sharedWaterMismatches"] = levelMismatches; result[U"maximumSharedWaterGap"] = maximumSharedWaterGap;
		result[U"sharedWaterExamples"] = sharedWaterExamples; result[U"interiorTerminalExamples"] = terminalExamples;
		result[U"oceanMouths"] = oceanMouths; result[U"nonzeroOceanMouths"] = nonzeroMouths;
		result[U"boundaryOutlets"] = boundaryOutlets; result[U"interiorTerminals"] = inlandTerminals;
		result[U"unresolved"] = rivers.statistics().unresolved; result[U"exhausted"] = rivers.statistics().exhausted;
		return result;
	}

	/// @brief Contract failures remain red; diagnostics do not turn known missing fixes into accepted behavior.
	void expectFoundationGraph(TestContext& context, const JSON& result)
	{
		context.expect(result[U"reaches"].get<int64>() > 0, U"The fixture must emit a nonempty river graph");
		for (const String key : {U"nonfinite", U"outOfBounds", U"uphill", U"splitOutflows", U"cyclicOrBlockedNodes",
			U"sharedWaterMismatches", U"nonzeroOceanMouths", U"interiorTerminals", U"unresolved", U"exhausted"})
		{
			context.expectEqual(result[key].get<int64>(), 0, U"River foundation requires zero {}"_fmt(key));
		}
		context.expect(result[U"oceanMouths"].get<int>() + result[U"boundaryOutlets"].get<int>() > 0,
			U"A nonempty acyclic network must drain to sea or the exact map boundary");
	}

}

void registerRiverGenerationTests(TestRunner& runner)
{
	runner.add(U"Rivers.FollowsNegativeGradient", [](TestContext& context)
	{
		const auto valley=[](double z) { return 4096+600*Sin(z/1800); };
		const auto height=[&](double x,double z) { return z*.012+Square(x-valley(z))*.00008-8; };
		RiverNetwork river; river.generate(8192,16384,height);
		double sum=0,run=0; int uphill=0;
		for (const auto& reach : river.reaches)
		{
			const Vec2 a{reach.start.x,reach.start.z},b{reach.end.x,reach.end.z},p=(a+b)*.5;
			const double side=p.x-valley(p.y);
			const Vec2 downhill{-side*.00016, -.012+side*.00016*(600.0/1800)*Cos(p.y/1800)};
			const double length=a.distanceFrom(b);
			if (length>.1 && downhill.lengthSq()>1e-10) { sum+=(b-a).normalized().dot(downhill.normalized())*length; run+=length; }
			uphill+=height(b.x,b.y)>height(a.x,a.y)+1e-7;
		}
		JSON report; report[U"meanGradientAlignment"]=sum/Max(1.0,run); report[U"length"]=run; report[U"terrainUphill"]=uphill;
		report.save(U"TestResults/river_gradient.json");
		context.expect(run>8000 && sum/run>.98, U"完成流路は各地点の -grad f と一致する");
		context.expectEqual(uphill,0,U"川を後から曲げて斜面を逆流させない");
	});

	runner.add(U"Rivers.LowlandAndMountainCourse", [](TestContext& context)
	{
		const auto center = [](double) { return 4096.0; };
		const auto broadHeight = [](double x, double z) { const double side = Abs(x - 4096); return z * .001 + side * .0006 + side * side / 838860.8 - 4; };
		const auto mountainHeight = [](double x, double z) { return z * .025 + Abs(x - 4096) * .25 - 4; };
		RiverNetwork broad, mountain;
		broad.generate(8192, 16384, broadHeight); mountain.generate(8192, 16384, mountainHeight);
		const JSON plain = surveyTrunk(broad, center), gorge = surveyTrunk(mountain, center);
		JSON result; result[U"plain"] = plain; result[U"gorge"] = gorge;
		result[U"plainNetwork"] = surveyNetwork(broad); result[U"gorgeNetwork"] = surveyNetwork(mountain);
		result.save(U"TestResults/river_course.json");
		context.expect(plain[U"samples"].get<int>() > 180 && gorge[U"samples"].get<int>() > 180, U"Both fixtures contain a continuous main channel");
		context.expectNear(plain[U"sinuosity"].get<double>(), 1, .005, U"一定勾配の直線谷では人工的な蛇行を足さない");
		context.expect(plain[U"rmsOffset"].get<double>() < 8, U"平野でも谷底の最急降下を追う");
		context.expect(gorge[U"rmsOffset"].get<double>() < 20, U"The same bends must not cut across steep valley walls");
		context.expectEqual(result[U"plainNetwork"][U"uphill"].get<int>(), 0, U"Bends preserve downstream water level");
	});

	runner.add(U"Rivers.CurvedValleyAndRepeatability", [](TestContext& context)
	{
		const auto valley = [](double z) { return 4096 + 450 * Sin(z / 1500) + 80 * Sin(z / 510); };
		const auto height = [&](double x, double z) { return z * .009 + Abs(x - valley(z)) * .24 - 4; };
		RiverNetwork first, same, different;
		first.generate(8192, 16384, height); same.generate(8192, 16384, height);
		const JSON measured = surveyTrunk(first, valley);
		measured.save(U"TestResults/river_curved_valley.json");
		context.expect(measured[U"samples"].get<int>() > 180, U"The curved valley has a continuous main stream");
		context.expect(measured[U"rmsOffset"].get<double>() < 45, U"Mountain streams follow the curved valley floor");
		context.expectEqual(same.reaches.size(), first.reaches.size(), U"Same terrain and seed preserve sample count");
		bool identical = same.reaches.size() == first.reaches.size();
		for (size_t id = 0; identical && id < first.reaches.size(); ++id)
		{
			identical = same.reaches[id].start == first.reaches[id].start && same.reaches[id].end == first.reaches[id].end;
		}
		context.expect(identical, U"Repeat generation preserves exact geometry and water levels");
		const auto plain = [](double x, double z) { return z * .001 + Abs(x - 4096) * .004 - 4; };
		first.generate(8192, 16384, plain); different.generate(8192, 16384, plain);
		context.expectNear(surveyTrunk(first, [](double) { return 4096.0; })[U"rmsOffset"].get<double>(),
			surveyTrunk(different, [](double) { return 4096.0; })[U"rmsOffset"].get<double>(), 1e-9,
			U"同じ地形なら同じ勾配を流れ、乱数で別の方向へ曲げない");
	});

	runner.add(U"Rivers.Foundation.NonGridBoundaryOutflows", [](TestContext& context)
	{
		constexpr double kWidth = 16337, kDepth = 11983, kOutletHeight = 120, kDownstreamGrade = .01;
		JSON report;
		for (int side = 0; side < 4; ++side)
		{
			const auto height = [side](double x, double z)
			{
				const double along = side < 2 ? x : z;
				const double extent = side < 2 ? kWidth : kDepth;
				const double across = side < 2 ? z - kDepth * .5 : x - kWidth * .5;
				return kOutletHeight + kDownstreamGrade * (side % 2 == 0 ? along : extent - along) + Square(across) * .00008;
			};
			RiverNetwork rivers; rivers.generate(kWidth, kDepth, height);
			JSON result = surveyFoundationGraph(rivers, kWidth, kDepth, [](double, double) { return false; });
			expectFoundationGraph(context, result);
			HashSet<Vec2> starts;
			for (const auto& reach : rivers.reaches) { starts.insert({reach.start.x, reach.start.z}); }
			double maximumOutletLevelError = 0;
			int intendedOutlets = 0;
			for (const auto& reach : rivers.reaches)
			{
				if (starts.contains(Vec2{reach.end.x, reach.end.z})) { continue; }
				const double coordinate = side < 2 ? reach.end.x : reach.end.z;
				const double expectedEdge = side % 2 == 0 ? 0 : (side < 2 ? kWidth : kDepth);
				context.expectNear(coordinate, expectedEdge, 1e-7, U"A fractional final drainage cell reaches its intended finite map edge");
				++intendedOutlets;
				maximumOutletLevelError = Max(maximumOutletLevelError,
					Abs(reach.end.y - (height(reach.end.x, reach.end.z) - GenerationSettings::get().rivers_valleyWaterDepth)));
			}
			result[U"intendedOutlets"] = intendedOutlets; result[U"maximumOutletLevelError"] = maximumOutletLevelError;
			report[Format(side)] = result;
			context.expect(intendedOutlets > 0, U"Each cardinal orientation exercises at least one land-edge outlet");
			context.expectNear(maximumOutletLevelError, 0, 1e-6, U"An unobstructed land outlet retains its local ground-relative water datum");
		}
		report.save(U"TestResults/river_foundation_non_grid_boundaries.json");
	});

	runner.add(U"Rivers.Foundation.MarineMouthStaysAtSea", [](TestContext& context)
	{
		constexpr double kWidth = 11983, kDepth = 16337;
		const auto height = [](double x, double z) { return z * .01 + Square(x - kWidth * .5) * .00008 - 20; };
		const auto marine = [&](double x, double z) { return height(x, z) <= 0; };
		RiverNetwork rivers; rivers.generate(kWidth, kDepth, height, marine);
		const JSON report = surveyFoundationGraph(rivers, kWidth, kDepth, marine);
		report.save(U"TestResults/river_foundation_marine.json");
		expectFoundationGraph(context, report);
		context.expect(report[U"oceanMouths"].get<int>() > 0, U"The marine fixture must contain a sea-level mouth");
		context.expectEqual(report[U"boundaryOutlets"].get<int>(), 0, U"The enclosed valley drains through its marine mouth");
		context.expect(report[U"confluences"].get<int>() > 0, U"The marine fixture must exercise shared tributary water levels");
	});

	runner.add(U"Rivers.Foundation.LocalSpillwayBesideMarineCell", [](TestContext& context)
	{
		// A dry sub-grid minimum lies beside a marine drainage cell, below the main coarse-grid saddle.
		const auto height = [](double x, double z)
		{
			return z * .012 + Square(x - 4096) * .00008 - 8 + 6 * std::exp(-Square((z - 560) / 40));
		};
		const auto marine = [&](double x, double z) { return height(x, z) < 0; };
		RiverNetwork rivers; rivers.generate(8192, 16384, height, marine);
		const JSON report = surveyFoundationGraph(rivers, 8192, 16384, marine);
		report.save(U"TestResults/river_foundation_local_marine_spillway.json");
		expectFoundationGraph(context, report);
		context.expect(report[U"oceanMouths"].get<int>() > 0, U"A dry sub-grid minimum beside a marine cell must continue to real sea");
	});

	runner.add(U"Rivers.Foundation.ZeroHeightMarineFlat", [](TestContext& context)
	{
		// A connected zero-height marine flat is a valid sea-level outlet, unlike a positive dry point in the same coarse cell.
		const auto height = [](double x, double z)
		{
			return Max(0.0, (z - 1024) * .012 + Square(x - 4096) * .00008) + Min(0.0, (z - 256) * .012);
		};
		RiverNetwork rivers; rivers.generate(8192, 16384, height, [](double, double z) { return z <= 1024; });
		const JSON report = surveyFoundationGraph(rivers, 8192, 16384,
			[&](double x, double z) { return z <= 1024 && height(x, z) <= 0; });
		report.save(U"TestResults/river_foundation_zero_height_marine_flat.json");
		expectFoundationGraph(context, report);
		context.expect(report[U"oceanMouths"].get<int>() > 0, U"A connected marine coastal flat is a valid outlet at sea level");
	});

	runner.add(U"Rivers.Foundation.GeneratedTopology", [](TestContext& context)
	{
		JSON report;
		for (const uint64 seed : {uint64{7}, uint64{42}, uint64{130}, uint64{2026}})
		{
			World world;
			{
				const RiverDebugLogScope logScope;
				DebugLog::initialize(U"TestResults/river_foundation_generated_seed_{}.log"_fmt(seed));
				world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE); world.generateRivers();
			}
			const JSON result = surveyFoundationGraph(world.rivers(), WORLD_SIZE, WORLD_SIZE,
				[&](double x, double z) { return world.getBiome(static_cast<float>(x), static_cast<float>(z)) == BiomeType::Ocean; });
			report[Format(seed)] = result;
			expectFoundationGraph(context, result);
			context.expect(result[U"confluences"].get<int>() > 0, U"Seed {} must exercise a shared confluence"_fmt(seed));
			context.expect(result[U"oceanMouths"].get<int>() > 0, U"Seed {} retains a river-to-sea connection"_fmt(seed));
		}
		report.save(U"TestResults/river_foundation_generated_graphs.json");
	});

	runner.add(U"Rivers.Foundation.InlandBoundaryIsAnOutlet",[](TestContext& context)
	{
		const auto height=[](double x,double z)
		{
			return x*.001+50*(1+std::tanh((x-4096)/900))+80*std::exp(-Square((x-4096)/900))+Square(z-8192)*.000002-5;
		};
		RiverNetwork rivers; rivers.generate(16337,16337,height);
		int boundaryOutlets=0;
		double maximumCut=0;
		for (const auto& reach : rivers.reaches)
		{
			if (reach.end.x>16336.98 && reach.end.y>0) { ++boundaryOutlets; }
			const Vec3 middle=(reach.start+reach.end)*.5;
			maximumCut=Max(maximumCut,height(middle.x,middle.z)-middle.y);
			context.expect(reach.end.y<=reach.start.y,U"A boundary outlet does not reverse the water profile");
		}
		context.expect(boundaryOutlets>0,U"An inland catchment may leave the region at its own elevation");
		context.expect(maximumCut<20,U"The opposite catchment is not excavated through the mountain divide to reach this map's sea");
	});

	runner.add(U"Rivers.BaselineCarving.OverlappingBanksStayContinuous",[](TestContext& context)
	{
		RiverNetwork source;
		for (const Vec2 river : {Vec2{100,100},Vec2{250,20}})
		{
			RiverNetwork::Reach reach;
			reach.start={river.x,river.y,0}; reach.end={river.x,river.y,1000};
			reach.halfWidth=reach.endHalfWidth=40; reach.bankExtent=300; reach.alluvium=2;
			reach.bounds=RectF{river.x-340,-340,680,1680}; source.reaches << reach;
		}
		const auto rivers=source.subset(RectF{0,0,500,1000});
		double previous=rivers.carveHeight({0,500},200),maximumJump=0;
		for (int step=1;step<=5000;++step)
		{
			const double height=rivers.carveHeight({step*.1,500},200);
			maximumJump=Max(maximumJump,Abs(height-previous)); previous=height;
		}
		context.expect(maximumJump<1,U"Switching between overlapping river shoulders cannot jump to another water datum");
		for (const double edge : {142.0,208.0,292.0})
		{
			const double left=rivers.carveHeight({edge-.0001,500},102);
			const double right=rivers.carveHeight({edge+.0001,500},102);
			context.expect(Abs(right-left)<.002,U"Deposited sediment fades continuously at every overlapping wet boundary");
		}
	});

	runner.add(U"Rivers.GeneratedRegionCourseAudit", [](TestContext& context)
	{
		JSON results;
		Array<uint64> seeds{42, 2026};
		for (uint64 seed = 0; seed < 20; ++seed) { seeds << seed; }
		for (const uint64 seed : seeds)
		{
			World world; world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			const auto start = std::chrono::steady_clock::now(); world.generateRivers();
			const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			JSON result = surveyNetwork(world.rivers()); result[U"seed"] = seed;
			result[U"generationMs"] = elapsed;
			result[U"sourceCount"]=world.rivers().statistics().sources;
			result[U"spillways"]=world.rivers().statistics().spillways;
			result[U"unresolved"]=world.rivers().statistics().unresolved;
			result[U"selfIntersections"]=world.rivers().statistics().selfIntersections;
			result[U"exhausted"]=world.rivers().statistics().exhausted;
			const auto key=[](const Vec3& point)
			{
				return Point{static_cast<int>(Round(point.x*1000)),static_cast<int>(Round(point.z*1000))};
			};
			HashSet<Point> starts;
			int incised=0,alluvial=0,bars=0;
			for (const auto& reach : world.rivers().reaches)
			{
				starts.insert(key(reach.start));
				incised+=reach.incision>.01;
				alluvial+=reach.alluvium>.01;
				bars+=reach.barSide!=0;
			}
			result[U"incisedReaches"]=incised; result[U"alluvialReaches"]=alluvial; result[U"bars"]=bars;
			context.expect(incised>0 && alluvial>0,U"Stream power erodes the channel and carried sediment settles downstream");
			int raisedBanks=0;
			for (const auto& reach : world.rivers().reaches)
			{
				if (reach.alluvium<.2) { continue; }
				const Vec2 a{reach.start.x,reach.start.z},b{reach.end.x,reach.end.z},direction=b-a;
				if (direction.lengthSq()<1) { continue; }
				const Vec2 side=Vec2{-direction.y,direction.x}.normalized();
				const Vec2 bank=(a+b)*.5+side*(reach.halfWidth+30);
				const auto sample=world.rivers().nearest(bank);
				if (sample.reach<0 || sample.distance<sample.halfWidth+10) { continue; }
				const double original=sample.surface+2;
				raisedBanks+=world.rivers().carveHeight(bank,original)>original+.01;
			}
			result[U"raisedBanks"]=raisedBanks;
			context.expect(raisedBanks>0,U"Deposited sediment raises a low bank beside the channel");
			int mouths=0,boundaryOutlets=0,dryTerminals=0;
			JSON dryExamples;
			for (const auto& reach : world.rivers().reaches)
			{
				if (starts.contains(key(reach.end))) { continue; }
				const auto& end=reach.end;
				if (world.getBiome(static_cast<float>(end.x),static_cast<float>(end.z))==BiomeType::Ocean) { ++mouths; }
				else if (end.x<.02 || end.z<.02 || end.x>WORLD_SIZE-.02 || end.z>WORLD_SIZE-.02) { ++boundaryOutlets; }
				else
				{
					if (dryTerminals<5) { dryExamples[Format(dryTerminals)]=Array<double>{end.x,end.z,world.computeHeight(static_cast<float>(end.x),static_cast<float>(end.z))}; }
					++dryTerminals;
				}
			}
			result[U"dryExamples"]=dryExamples;
			result[U"boundaryOutlets"]=boundaryOutlets; result[U"oceanMouths"]=mouths; result[U"dryTerminals"]=dryTerminals;
			results[Format(seed)] = result;
			context.expect(mouths>0,U"Each generated region has a river mouth in the sea");
			context.expectEqual(dryTerminals,0,U"Every independent channel reaches the sea or a real land-boundary outlet");
			context.expect(!world.rivers().reaches.isEmpty(), U"Generated region has rivers");
			context.expectEqual(result[U"uphill"].get<int>(), 0, U"Every generated reach flows downstream");
			context.expectEqual(result[U"unconnectedCrossings"].get<int>(), 0, U"Independent river channels must not cross without a confluence");
			context.expect(result[U"maximumSegment"].get<double>() < 18, U"River banks have sufficiently short samples");
			context.expect(result[U"gridAlignedFraction"].get<double>() < .32, U"Terrain rivers should not inherit the drainage grid over most of their length");
		}
		results.save(U"TestResults/river_regions.json");
	});
}
