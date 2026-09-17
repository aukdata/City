#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/RiverNetwork.hpp"
#include "src/world/World.hpp"
#include <chrono>

namespace
{
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
			results[Format(seed)] = result;
			context.expect(!world.rivers().reaches.isEmpty(), U"Generated region has rivers");
			context.expectEqual(result[U"uphill"].get<int>(), 0, U"Every generated reach flows downstream");
			context.expectEqual(result[U"unconnectedCrossings"].get<int>(), 0, U"Independent river channels must not cross without a confluence");
			context.expect(result[U"maximumSegment"].get<double>() < 18, U"River banks have sufficiently short samples");
			context.expect(result[U"gridAlignedFraction"].get<double>() < .32, U"Terrain rivers should not inherit the drainage grid over most of their length");
		}
		results.save(U"TestResults/river_regions.json");
	});
}
