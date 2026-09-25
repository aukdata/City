#include "GenerationSettings.hpp"
#include "RoadAlignment.hpp"
#include "RoadDesignLimits.hpp"
#include "RailCostProfile.hpp"
#include "RoadConstructionCost.hpp"
#include "RoadPathfinder.hpp"
#include <queue>

namespace
{
	constexpr int kHeadings = 32;
	constexpr double kAngle = Math::TwoPi/kHeadings;
	
	
	

	Vec2 horizontal(Vec3 point) { return {point.x,point.z}; }
	Vec3 direction(int heading) { return {Cos(heading*kAngle),0,Sin(heading*kAngle)}; }

	/// @brief 設計面の高さを弦方向の平面とし、パラメータ速度による勾配の増幅を防ぐ。
	CubicBezier profile(Vec3 a,Vec3 b,Vec3 tangentA,Vec3 tangentB,double arm)
	{
		Vec3 first=a+tangentA*arm,second=b-tangentB*arm;
		const Vec2 span=horizontal(b-a);
		const auto height=[&](Vec3 p) { return a.y+(b.y-a.y)*horizontal(p-a).dot(span)/Max(1e-9,span.lengthSq()); };
		first.y=height(first); second.y=height(second);
		return {a,first,second,b};
	}

	/// @brief 地形回廊をC2連続のBスプラインにする。端点を固定し、曲がりを区間全体へ配分する。
	Array<CubicBezier> contourCurves(const Array<Vec3>& points)
	{
		Array<CubicBezier> curves;
		for (size_t i = 0; i + 1 < points.size(); ++i)
		{
			Vec3 b = points[i], c = points[i + 1];
			Vec3 a = i > 0 ? points[i - 1] : b * 2 - c;
			Vec3 d = i + 2 < points.size() ? points[i + 2] : c * 2 - b;
			a.y = b.y = c.y = d.y = 0;
			// 外挿した端の制御点で始終点を通す。内部は接線だけでなく曲率も共有する。
			const Vec3 start = i == 0 ? b : b + ((a - b) + (c - b)) / 6;
			const Vec3 end = i + 2 == points.size() ? c : c + ((b - c) + (d - c)) / 6;
			curves << CubicBezier{start, b.lerp(c, 1.0 / 3), b.lerp(c, 2.0 / 3), end};
		}
		return curves;
	}

	/// @brief 折れ線の各角だけを円弧で丸める。山を迂回する折り返し全体は縮めない。
	Array<CubicBezier> roundCorridor(const Array<Vec3>& points,double radius)
	{
		Array<CubicBezier> result;
		Vec3 previous=points.front(); previous.y=0;
		const auto straight=[&](Vec3 a,Vec3 b) { if (horizontal(b-a).length()>.01) { result << CubicBezier{a,a.lerp(b,1.0/3),a.lerp(b,2.0/3),b}; } };
		for (size_t i=1;i+1<points.size();++i)
		{
			Vec3 center=points[i],before=points[i-1],after=points[i+1]; center.y=before.y=after.y=0;
			Vec3 incoming=center-before,outgoing=after-center;
			if (incoming.lengthSq()<.01 || outgoing.lengthSq()<.01) { return {}; }
			const double available=Min(incoming.length(),outgoing.length())*.49;
			incoming=incoming.normalized(); outgoing=outgoing.normalized();
			const double angle=Acos(Clamp(incoming.dot(outgoing),-1.0,1.0));
			if (angle<.001) { continue; }
			const double tangent=radius*Tan(angle*.5),arm=4.0/3*radius*Tan(angle*.25);
			if (tangent>available) { return {}; }
			const Vec3 a=center-incoming*tangent,b=center+outgoing*tangent;
			straight(previous,a); result << CubicBezier{a,a+incoming*arm,b-outgoing*arm,b}; previous=b;
		}
		Vec3 end=points.back(); end.y=0; straight(previous,end);
		return result;
	}

	struct Landscape
	{
		const World& world;
		Vec2 lower,upper;
		RoadType type;
		double grade;
		TransportMode mode = TransportMode::Road;
		double maximumViaductHeight = Math::Inf;

		Optional<double> cost(const CubicBezier& curve, double* tunnelLength = nullptr) const
		{
			if (!RoadAlignment::respectsLimits(curve,type,mode)) { return none; }
			for (const Vec3 point : {curve.p0,curve.p3})
			{
				if (point.x<lower.x || point.z<lower.y || point.x>upper.x || point.z>upper.y) { return none; }
				if (!world.getChunk({static_cast<int>(point.x)/CHUNK_SIZE,static_cast<int>(point.z)/CHUNK_SIZE})) { return none; }
				const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)),water=world.waterSurfaceHeight(point.x,point.z);
				if (point.y-ground>maximumViaductHeight) { return none; }
				if (ground<water+GenerationSettings::get().crossings_waterBankMargin && ground-point.y<=RoadConstructionCost::MaximumCut() && point.y<water+5.5) { return none; }
			}
			const int count=Max(2,static_cast<int>(std::ceil(curve.totalLength/GenerationSettings::get().routing_sampleSpacing)));
			double total=0, tunnelRun=tunnelLength ? *tunnelLength : 0;
			for (int i=0;i<count;++i)
			{
				const Vec3 point=curve.positionAt(curve.totalLength*(static_cast<float>(i)+.5f)/count);
				if (point.x<lower.x || point.z<lower.y || point.x>upper.x || point.z>upper.y) { return none; }
				if (!world.getChunk({static_cast<int>(point.x)/CHUNK_SIZE,static_cast<int>(point.z)/CHUNK_SIZE})) { return none; }
				const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				if (point.y-ground>maximumViaductHeight) { return none; }
				const double water = world.waterSurfaceHeight(point.x, point.z);
				if (ground < water + GenerationSettings::get().crossings_waterBankMargin && ground - point.y <= RoadConstructionCost::MaximumCut() && point.y < water + 5.5) { return none; }
				total += RoadConstructionCost::segment(point.y, ground, water, curve.totalLength/count, tunnelRun);
			}
			if (!std::isfinite(total)) { return none; }
			if (tunnelLength) { *tunnelLength=tunnelRun; }
			return total;
		}
	};

	struct SearchNode
	{
		Vec3 point;
		int heading=0;
		int parent=-1;
		double cost=0;
		uint64 key=0;
		bool expanded=false;
		double tunnelLength=0;
	};
}

bool RoadAlignment::respectsLimits(const CubicBezier& curve,RoadType type,TransportMode mode)
{
	const auto limits=RoadDesignLimits::forType(type,mode);
	if (curve.minimumHorizontalRadius()+.001<limits.minimumRadius) { return false; }
	for (int i=0;i<=16;++i)
	{
		const Vec3 tangent=curve.tangent(i/16.0f);
		if (Abs(tangent.y)>horizontal(tangent).length()*limits.maximumGrade+1e-8) { return false; }
	}
	return true;
}

Array<CubicBezier> RoadAlignment::fit(const Array<Vec3>& points)
{
	Array<CubicBezier> curves;
	for (size_t i=0;i+1<points.size();++i)
	{
		const Vec3 a=points[i],b=points[i+1];
		Vec3 before=i>0 ? b-points[i-1] : b-a;
		Vec3 after=i+2<points.size() ? points[i+2]-a : b-a;
		before.y=after.y=0;
		if (before.lengthSq()<1e-8 || after.lengthSq()<1e-8) { return {}; }
		curves << profile(a,b,before.normalized(),after.normalized(),horizontal(b-a).length()/3);
	}
	return curves;
}

double RoadAlignment::constructionCost(const World& world, const Array<CubicBezier>& curves, RoadType type, TransportMode mode)
{
	const Landscape landscape{world, {0, 0}, {WORLD_SIZE - .01, WORLD_SIZE - .01}, type,
		RoadDesignLimits::forType(type,mode).maximumGrade,mode};
	double total = 0, tunnelLength = 0;
	Optional<Vec3> previous;
	for (const auto& curve : curves)
	{
		CubicBezier oriented=curve;
		if (previous && previous->distanceFromSq(curve.p3)<previous->distanceFromSq(curve.p0)) { oriented=CubicBezier{curve.p3,curve.p2,curve.p1,curve.p0}; }
		if (previous && previous->distanceFromSq(oriented.p0)>1) { tunnelLength=0; }
		const auto cost = landscape.cost(oriented, &tunnelLength);
		previous=oriented.p3;
		if (!cost) { return Math::Inf; }
		total += *cost;
	}
	return total;
}

double RoadAlignment::maximumClearance(const World& world,const Array<CubicBezier>& curves)
{
	double result=0;
	for (const auto& curve : curves)
	{
		const int count=Max(2,static_cast<int>(Ceil(curve.totalLength/GenerationSettings::get().routing_sampleSpacing)));
		for (int i=0;i<=count;++i)
		{
			const auto p=curve.positionAt(curve.totalLength*static_cast<float>(i)/count);
			result=Max(result,p.y-world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)));
		}
	}
	return result;
}

/// @brief 回廊から離れ過ぎない範囲で平面を丸め、実地盤上で勾配の上下包絡線を解く。
static Optional<RoadAlignment::Result> fitTerrainCorridor(const World& world, const Array<Vec3>& input, RoadType type, TransportMode mode, bool continuous)
{
	if (input.size() < 2) { return none; }
	const auto limits = RoadDesignLimits::forType(type, mode);
	const auto& settings = GenerationSettings::get();
	const double grade = limits.maximumGrade * settings.routing_gradeReserve;
	constexpr double kMaximumShiftRadiusRatio = .25;
	const double maximumShift = Max(settings.routing_terrainProfileSpacing, limits.minimumRadius * kMaximumShiftRadiusRatio);
	Array<Vec3> points = input;
	int radiusPasses=0, profilePasses=0;
	Optional<RoadAlignment::Result> best;
	for (int pass = 0; pass <= settings.routing_terrainSmoothingPasses; ++pass)
	{
		if (pass > 0)
		{
			Array<Vec3> next = points;
			for (size_t i=1; i+1<points.size(); ++i)
			{
				next[i] = points[i].lerp((points[i-1]+points[i+1])*.5, .25);
				const Vec2 shift = horizontal(next[i] - input[i]);
				if (continuous && shift.length() > maximumShift)
				{
					const Vec2 bounded = horizontal(input[i]) + shift.normalized() * maximumShift;
					next[i].x = bounded.x; next[i].z = bounded.y;
				}
				// 高さまで隣接点で平均すると、谷を横切る空中道路に変わる。
				next[i].y = world.sampleHeight(static_cast<float>(next[i].x), static_cast<float>(next[i].z));
			}
			points = std::move(next);
		}
		if (pass % 4 != 0) { continue; }
		auto horizontalCurves = continuous ? contourCurves(points) : roundCorridor(points,limits.minimumRadius*1.02);
		if (!continuous && horizontalCurves.isEmpty()) { horizontalCurves=RoadAlignment::fit(points); }
		bool radiusValid = !horizontalCurves.isEmpty();
		for (const auto& curve : horizontalCurves) { radiusValid &= curve.minimumHorizontalRadius()+.001 >= limits.minimumRadius; }
		if (!radiusValid) { continue; }
		++radiusPasses;
		Array<CubicBezier> pieces;
		for (auto curve : horizontalCurves)
		{
			const int count = Max(1,static_cast<int>(Ceil(curve.totalLength/settings.routing_terrainProfileSpacing)));
			for (int remaining=count; remaining>1; --remaining)
			{
				auto pair=curve.split(1.0f/remaining); pieces << pair.first; curve=pair.second;
			}
			pieces << curve;
		}
		Array<double> requiredWater(pieces.size()+1,-Math::Inf);
		for (size_t i=0;i<pieces.size();++i)
		{
			const auto& piece=pieces[i];
			const int samples=Max(1,static_cast<int>(Ceil(piece.totalLength/settings.crossings_waterSampleStep)));
			for (int sample=0;sample<samples;++sample)
			{
				const Vec3 p=piece.positionAt(piece.totalLength*(static_cast<float>(sample)+.5f)/samples);
				const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
				const double water=world.waterSurfaceHeight(p.x,p.z);
				if (ground>=water+settings.crossings_waterBankMargin) { continue; }
				const double safe=water+Max(5.5,settings.crossings_waterClearance)+1;
				requiredWater[i]=Max(requiredWater[i],safe);
				requiredWater[i+1]=Max(requiredWater[i+1],safe);
			}
		}
		Array<double> levels, lower, upper, rises;
		for (size_t i=0; i<=pieces.size(); ++i)
		{
			const Vec3 p=i<pieces.size() ? pieces[i].p0 : pieces.back().p3;
			const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)),water=world.waterSurfaceHeight(p.x,p.z);
			const bool wet=ground<water+settings.crossings_waterBankMargin;
			const double target=Max(wet ? water+Max(5.5,settings.crossings_waterClearance)+1 : ground,requiredWater[i]);
			levels << target;
			// Keep short excavations and bridges available while following the sampled terrain.
			lower << Max(wet ? target : ground-RoadConstructionCost::MaximumCut()*2,requiredWater[i]);
			upper << Max(target,ground+RoadConstructionCost::MaximumFill()*2);
			if (i>0) { rises << horizontal(p-pieces[i-1].p0).length()*grade; }
		}
		lower.front()=upper.front()=levels.front()=input.front().y;
		lower.back()=upper.back()=levels.back()=input.back().y;
		// 前後2走査で勾配制約の包絡線を確定。固定端点も同じ制約に含める。
		for (size_t i=1; i<levels.size(); ++i) { lower[i]=Max(lower[i],lower[i-1]-rises[i-1]); upper[i]=Min(upper[i],upper[i-1]+rises[i-1]); }
		for (size_t i=levels.size()-1; i-->0;) { lower[i]=Max(lower[i],lower[i+1]-rises[i]); upper[i]=Min(upper[i],upper[i+1]+rises[i]); }
		bool feasible=true;
		for (size_t i=0; i<levels.size(); ++i)
		{
			if (lower[i]>upper[i]+1e-6) { feasible=false; break; }
			levels[i]=Clamp(levels[i],lower[i],upper[i]);
		}
		if (!feasible)
		{
			if (pass==0 || pass==settings.routing_terrainSmoothingPasses)
			{
				double excess=0; size_t worst=0;
				for (size_t i=0;i<levels.size();++i) { if (lower[i]-upper[i]>excess) { excess=lower[i]-upper[i];worst=i; } }
				DBG_LOG(U"[RoadTerrainEnvelope] points={} pass={} excess={} index={} first={} last={}"_fmt(points.size(),pass,excess,worst,points.front(),points.back()));
			}
			continue;
		}
		for (size_t i=1; i<levels.size(); ++i) { levels[i]=Min(levels[i],levels[i-1]+rises[i-1]); }
		for (size_t i=levels.size()-1; i-->0;) { levels[i]=Min(levels[i],levels[i+1]+rises[i]); }
		++profilePasses;
		Array<CubicBezier> curves;
		for (size_t i=0; i<pieces.size(); ++i)
		{
			auto c=pieces[i]; c.p0.y=levels[i]; c.p3.y=levels[i+1];
			const Vec2 span=horizontal(c.p3-c.p0);
			const auto height=[&](Vec3 p) { return c.p0.y+(c.p3.y-c.p0.y)*horizontal(p-c.p0).dot(span)/Max(1e-9,span.lengthSq()); };
			c.p1.y=height(c.p1); c.p2.y=height(c.p2);
			curves << CubicBezier{c.p0,c.p1,c.p2,c.p3};
		}
		const double cost=RoadAlignment::constructionCost(world,curves,type,mode);
		if (std::isfinite(cost) && (!best || cost<best->cost)) { best=RoadAlignment::Result{curves,cost,0}; }
	}
	if (!best) { DBG_LOG(U"[RoadTerrainFitRejected] continuous={} points={} radiusPasses={} profilePasses={}"_fmt(continuous,points.size(),radiusPasses,profilePasses)); }
	return best;
}

Optional<RoadAlignment::Result> RoadAlignment::fitTerrain(const World& world, const Array<Vec3>& input, RoadType type, TransportMode mode)
{
	const Stopwatch timer{StartImmediately::Yes};
	// 円弧と直線の安い組合せが、成立する緩やかな地形曲線を毎回置き換えないようにする。
	auto result = fitTerrainCorridor(world, input, type, mode, true);
	const bool continuous = result.has_value();
	// 狭い峠・急な折り返しで連続曲率を確保できない場合も、地形検査済みの接続を残す。
	if (!result) { result = fitTerrainCorridor(world, input, type, mode, false); }
	if (result)
	{
		DBG_LOG(U"[RoadTerrainFit] continuous={} points={} curves={} cost={} from={} to={} elapsedMs={}"_fmt(
			continuous, input.size(), result->curves.size(), result->cost, input.front(), input.back(), timer.msF()));
	}
	return result;
}

Optional<RoadAlignment::Result> RoadAlignment::find(const World& world,Vec3 start,Vec3 goal,RoadType type,int expansionLimit,TransportMode mode,double maximumViaductHeight)
{
	const auto limits=RoadDesignLimits::forType(type,mode);
	const double distance=horizontal(goal-start).length();
	if (distance<2) { return none; }
	const double grade=limits.maximumGrade*GenerationSettings::get().routing_gradeReserve;
	const double step=Max(GenerationSettings::get().routing_minimumStep,Max(limits.minimumRadius*GenerationSettings::get().routing_radiusStepRatio,Min(GenerationSettings::get().routing_maximumStep,distance/GenerationSettings::get().routing_distanceStepDivisor)));
	const double margin=Max(GenerationSettings::get().routing_minimumSearchMargin,Max(limits.minimumRadius*GenerationSettings::get().routing_radiusMarginRatio,Max(distance*(distance>GenerationSettings::get().network_mediumRouteThreshold ? GenerationSettings::get().routing_longDistanceMarginRatio : GenerationSettings::get().routing_distanceMarginRatio),Abs(goal.y-start.y)/grade*GenerationSettings::get().routing_riseMarginRatio)));
	const Landscape landscape{world,
		{Max(0.0,Min(start.x,goal.x)-margin),Max(0.0,Min(start.z,goal.z)-margin)},
		{Min(static_cast<double>(WORLD_SIZE)-.01,Max(start.x,goal.x)+margin),Min(static_cast<double>(WORLD_SIZE)-.01,Max(start.z,goal.z)+margin)},type,grade,mode,maximumViaductHeight};
	const bool requireTerrainCorridor = mode == TransportMode::Road && std::isfinite(maximumViaductHeight);
	const Vec3 straight=Vec3{goal.x-start.x,0,goal.z-start.z}.normalized();
	const CubicBezier direct=profile(start,goal,straight,straight,distance/3);
	const auto directCost=landscape.cost(direct);
	// On a flat unobstructed site, a straight surface road is already the minimum-length solution.
	if (directCost && *directCost<=direct.totalLength*(1+1e-6)) { return Result{{direct},*directCost,0}; }

	Optional<Result> incumbent;
	// 自動生成では、地形を検証していない直線を探索失敗時の代替経路にしない。
	if (directCost && !requireTerrainCorridor) { incumbent = Result{{direct}, *directCost, 0}; }
	// A cheap, terrain-following corridor gives the spatial search an early upper bound.
	RoadPathfinder coarse;
	const float coarseStep = static_cast<float>(Max(GenerationSettings::get().routing_coarseMinimumStep, Max(limits.minimumRadius * GenerationSettings::get().routing_coarseRadiusRatio, distance / GenerationSettings::get().routing_distanceStepDivisor)));
	const int width = Max(2, static_cast<int>(Ceil((landscape.upper.x - landscape.lower.x) / coarseStep)));
	const int depth = Max(2, static_cast<int>(Ceil((landscape.upper.y - landscape.lower.y) / coarseStep)));
	coarse.setup(world, landscape.lower, width, depth, coarseStep); coarse.setRoadType(type); coarse.setRailwayRouting(mode == TransportMode::Rail);
	const auto path = coarse.findPath(coarse.worldToGrid(static_cast<float>(start.x), static_cast<float>(start.z)),
		coarse.worldToGrid(static_cast<float>(goal.x), static_cast<float>(goal.z)));
	if (path.size() > 1)
	{
		auto points = coarse.samplePath(path, 3); points.front() = start; points.back() = goal;
		const auto curves = fit(points);
		const double cost = constructionCost(world, curves, type, mode);
		if (!requireTerrainCorridor && std::isfinite(cost) && maximumClearance(world,curves)<=maximumViaductHeight && (!incumbent || cost < incumbent->cost)) { incumbent = Result{curves, cost, 0}; }
		if (mode == TransportMode::Road)
		{
			if (const auto terrain = fitTerrain(world, points, type, mode); terrain && maximumClearance(world,terrain->curves)<=maximumViaductHeight && (!incumbent || terrain->cost < incumbent->cost)) { incumbent = terrain; }
		}
	}

	if (mode == TransportMode::Road)
	{
		coarse.setSurfaceOnly(true);
		const auto groundPath = coarse.findPath(coarse.worldToGrid(static_cast<float>(start.x),static_cast<float>(start.z)),
			coarse.worldToGrid(static_cast<float>(goal.x),static_cast<float>(goal.z)));
		DBG_LOG(U"[RoadGroundCorridor] cells={} step={} margin={}"_fmt(groundPath.size(),coarseStep,margin));
		if (groundPath.size()>1)
		{
			auto points=coarse.samplePath(groundPath,3); points.front()=start; points.back()=goal;
			if (const auto terrain=fitTerrain(world,points,type,mode); terrain && maximumClearance(world,terrain->curves)<=maximumViaductHeight && (!incumbent || terrain->cost<incumbent->cost)) { incumbent=terrain; }
		}
	}
	bool refining = incumbent && incumbent->curves.size() > 1;
	const double cell=step*GenerationSettings::get().routing_stateCellRatio;
	const auto keyFor=[&](Vec3 p,int heading)
	{
		const uint64 x=static_cast<uint64>(std::floor((p.x-landscape.lower.x)/cell));
		const uint64 z=static_cast<uint64>(std::floor((p.z-landscape.lower.y)/cell));
		const uint64 height=static_cast<uint64>(static_cast<int>(std::round(p.y/GenerationSettings::get().routing_heightStep))+32768);
		return (x<<45)|(z<<29)|(height<<5)|static_cast<uint64>(heading);
	};
	const auto heuristic=[&](Vec3 p) { return Max(horizontal(goal-p).length(),Abs(goal.y-p.y)/limits.maximumGrade); };
	Array<SearchNode> nodes;
	HashTable<uint64,Array<int>> best;
	using Entry=std::pair<double,int>;
	std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending, lowerBounds;
	const auto enqueue=[&](Vec3 point,int heading,int parent,double cost,double tunnelLength)
	{
		if (incumbent && cost + heuristic(point) >= incumbent->cost) { return; }
		const uint64 key=keyFor(point,heading);
		auto& labels=best[key];
		// Cost and uninterrupted tunnel length are both relevant to future cost.
		for (const int label : labels)
		{
			const auto& other=nodes[label];
			if (other.cost<=cost && other.tunnelLength<=tunnelLength) { return; }
		}
		labels.remove_if([&](int label)
		{
			auto& other=nodes[label];
			if (cost<=other.cost && tunnelLength<=other.tunnelLength) { other.expanded=true; return true; }
			return false;
		});
		const int id=static_cast<int>(nodes.size());
		nodes << SearchNode{point,heading,parent,cost,key,false,tunnelLength}; labels << id;
		pending.emplace(cost+heuristic(point)*GenerationSettings::get().routing_initialSearchWeight,id);
		lowerBounds.emplace(cost+heuristic(point),id);
	};
	for (int heading=0;heading<kHeadings;++heading) { enqueue(start,heading,-1,0,0); }
	int expanded=0,goalChecks=0,fitFailures=0; double nearest=distance,highest=start.y;
	while (expanded < expansionLimit)
	{
		// First obtain a feasible route; then minimize the shared cost with the admissible lower bound.
		auto& queue = refining ? lowerBounds : pending;
		if (queue.empty()) { break; }
		const auto [estimate, id] = queue.top(); queue.pop();
		const SearchNode current = nodes[id];
		if (current.expanded) { continue; }
		if (refining && incumbent && estimate >= incumbent->cost) { break; }
		nodes[id].expanded = true;
		++expanded; nearest=Min(nearest,horizontal(goal-current.point).length()); highest=Max(highest,current.point.y);
		const double remaining=horizontal(goal-current.point).length();
		if (remaining>1 && remaining<Max(step*5, limits.minimumRadius*3))
		{
			++goalChecks;
			const Vec3 approach=Vec3{goal.x-current.point.x,0,goal.z-current.point.z}.normalized();
			const CubicBezier finish=profile(current.point,goal,direction(current.heading),approach,remaining/3);
			double finishTunnel=current.tunnelLength;
			if (const auto finishCost=landscape.cost(finish,&finishTunnel))
			{
				Array<int> chain;
				for (int index=id;index>=0;index=nodes[index].parent) { chain << index; }
				chain.reverse();
				Array<CubicBezier> curves;
				for (size_t i=1;i<chain.size();++i)
				{
					const auto& a=nodes[chain[i-1]]; const auto& b=nodes[chain[i]];
					curves << profile(a.point,b.point,direction(a.heading),direction(b.heading),horizontal(b.point-a.point).length()/3);
				}
				curves << finish;
				const double cost=constructionCost(world,curves,type,mode);
				const bool valid=std::isfinite(cost);
				if (valid && (!incumbent || cost < incumbent->cost)) { incumbent = Result{curves, cost, expanded}; refining = true; }
				if (valid) { continue; }
				++fitFailures;
			}
		}
		for (int turn=-3;turn<=3;++turn)
		{
			const int heading=(current.heading+turn+kHeadings)%kHeadings;
			const double angle=turn*kAngle;
			const Vec3 forward=direction(current.heading),endDirection=direction(heading);
			Vec3 end=current.point;
			if (turn==0) { end+=forward*step; }
			else
			{
				const double radius=step/angle;
				end.x+=radius*(endDirection.z-forward.z);
				end.z+=radius*(forward.x-endDirection.x);
			}
			if (end.x<landscape.lower.x || end.z<landscape.lower.y || end.x>landscape.upper.x || end.z>landscape.upper.y) { continue; }
			const double rise=horizontal(end-current.point).length()*grade;
			const double terrain=world.sampleHeight(static_cast<float>(end.x),static_cast<float>(end.z));
			const double water=world.waterSurfaceHeight(end.x,end.z);
			const double desired=terrain<water+GenerationSettings::get().crossings_waterBankMargin ? water+GenerationSettings::get().crossings_waterClearance : terrain;
			Array<double> heights{Clamp(desired,current.point.y-rise,current.point.y+rise)};
			for (const double fraction : {-1.0,0.0,1.0})
			{
				const double level=std::round((current.point.y+rise*fraction)/GenerationSettings::get().routing_heightStep)*GenerationSettings::get().routing_heightStep;
				if (Abs(level-current.point.y)<=rise+1e-8 && !heights.contains(level)) { heights << level; }
			}
			for (double height : heights)
			{
				end.y=height;
				const CubicBezier curve=profile(current.point,end,forward,endDirection,horizontal(end-current.point).length()/3);
				double tunnelLength=current.tunnelLength;
				if (const auto cost=landscape.cost(curve,&tunnelLength))
				{
					enqueue(end,heading,id,current.cost+*cost,tunnelLength);
				}
			}
		}
	}
	if (incumbent) { incumbent->expanded = expanded; return incumbent; }
	DBG_LOG(U"[RoadAlignmentSearch] nearest={} highest={} goals={} fitFailures={} nodes={}"_fmt(nearest,highest,goalChecks,fitFailures,nodes.size()));
	DBG_LOG(U"[RoadAlignmentBlocked] from=({}, {}, {}) to=({}, {}, {}) expanded={} limit={}"_fmt(start.x,start.y,start.z,goal.x,goal.y,goal.z,expanded,expansionLimit));
	return none;
}

void RoadAlignment::repairSteepEdges(RoadNetwork& roads,const World& world)
{
	// Fit one continuous vertical profile between junctions before deciding whether its route needs a detour.
	struct Corridor { Array<int> edges; int start,end; };
	Array<Corridor> corridors; HashTable<int,size_t> corridorForEdge;
	HashSet<int> visited;
	for (const auto& first : roads.edges())
	{
		if (first.id<0 || visited.contains(first.id)) { continue; }
		Array<int> chain{first.id},points{first.nodeA,first.nodeB}; visited.insert(first.id);
		for (int end=0;end<2;++end)
		{
			for (;;)
			{
				const int id=end==0 ? points.front() : points.back(); const auto* node=roads.getNode(id);
				if (node->attachments.size()!=2) { break; }
				Optional<int> next;
				for (const auto& attachment : node->attachments) { if (!visited.contains(attachment.edgeId)) { next=attachment.edgeId; } }
				if (!next || roads.getEdge(*next)->roadType!=first.roadType) { break; }
				visited.insert(*next); const auto* edge=roads.getEdge(*next); const int other=edge->nodeA==id ? edge->nodeB : edge->nodeA;
				if (end==0) { chain.insert(chain.begin(),*next); points.insert(points.begin(),other); }
				else { chain << *next; points << other; }
			}
		}
		for (int id : chain) { corridorForEdge[id]=corridors.size(); }
		corridors << Corridor{chain,points.front(),points.back()};
		if (chain.size()<2) { continue; }
		const double grade=RoadDesignLimits::forType(first.roadType).maximumGrade*GenerationSettings::get().routing_finalGradeReserve;
		bool needsProfile=false;
		for (size_t i=1;i<points.size();++i)
		{
			const Vec3 delta=roads.getNode(points[i])->position-roads.getNode(points[i-1])->position;
			needsProfile |= Abs(delta.y)>horizontal(delta).length()*grade+.001;
		}
		if (!needsProfile) { continue; }
		Array<RailCostProfile::Sample> samples;
		for (const int id : points)
		{
			const Vec3 point=roads.getNode(id)->position;
			const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)),water=world.waterSurfaceHeight(point.x,point.z);
			const bool wet=ground<water+GenerationSettings::get().crossings_waterBankMargin;
			samples << RailCostProfile::Sample{horizontal(point), ground, ground-200, Max(ground+120, water+12),
				wet ? water+GenerationSettings::get().crossings_waterClearance : -1e9, wet ? ground-GenerationSettings::get().roads_maximumCut-.01 : 1e9};
		}
		double start=roads.getNode(points.front())->position.y,goal=roads.getNode(points.back())->position.y;
		double run=0;
		for (size_t i=1;i<samples.size();++i) { run+=samples[i].position.distanceFrom(samples[i-1].position); }
		const double excess=Abs(goal-start)-run*grade;
		if (excess>0 && excess<3 && roads.getNode(points.front())->attachments.size()==1 && roads.getNode(points.back())->attachments.size()==1)
		{
			const double adjustment=(excess+.05)*.5*(goal>start ? 1 : -1);
			start+=adjustment; goal-=adjustment;
		}
		samples.front().minimum=samples.front().maximum=start; samples.back().minimum=samples.back().maximum=goal;
		auto result = RailCostProfile::solve(samples, start, goal, grade, [&](const RailCostProfile::Sample& sample, double elevation)
		{
			return RoadConstructionCost::unit(elevation, sample.ground, world.waterSurfaceHeight(sample.position.x, sample.position.y));
		});
		if (!result.feasible && Abs(goal-start)<=run*grade)
		{
			// A half-metre height lattice can miss a valid slope close to its bound.
			// Check the continuous straight profile before declaring the corridor infeasible.
			result.heights.clear(); double along=0; bool valid=true;
			for (size_t i=0;i<samples.size();++i)
			{
				if (i>0) { along+=samples[i].position.distanceFrom(samples[i-1].position); }
				const double level=Math::Lerp(start,goal,along/Max(1e-9,run));
				valid &= level>=samples[i].minimum-1e-8 && level<=samples[i].maximum+1e-8
					&& (level>=samples[i].roadClearance || level<=samples[i].underpass);
				result.heights << level;
			}
			result.feasible=valid;
		}
		if (!result.feasible) { continue; }
		for (size_t i=0;i<points.size();++i) { roads.getNode(points[i])->position.y=result.heights[i]; }
		for (int id : chain)
		{
			auto* edge=roads.getEdge(id); const auto curve=roads.getBezier(id);
			const Vec2 span=horizontal(curve->p3-curve->p0);
			const auto level=[&](Vec3 p) { return curve->p0.y+(curve->p3.y-curve->p0.y)*horizontal(p-curve->p0).dot(span)/Max(1e-9,span.lengthSq()); };
			edge->ctrlA.y=level(edge->ctrlA); edge->ctrlB.y=level(edge->ctrlB); edge->designGrade=true;
		}
	}
	Array<size_t> candidates;
	for (const auto& edge : roads.edges())
	{
		if (edge.id<0) { continue; }
		const Vec3 a=roads.getNode(edge.nodeA)->position,b=roads.getNode(edge.nodeB)->position;
		if (Abs(b.y-a.y)>horizontal(b-a).length()*RoadDesignLimits::forType(edge.roadType).maximumGrade*GenerationSettings::get().routing_finalGradeReserve+6)
		{
			const size_t corridor=corridorForEdge[edge.id];
			if (!candidates.contains(corridor)) { candidates << corridor; }
		}
	}
	for (size_t i = 0; i < corridors.size(); ++i)
	{
		if (candidates.contains(i)) { continue; }
		const auto& corridor = corridors[i];
		Array<CubicBezier> curves; double length = 0;
		for (const int id : corridor.edges) { if (const auto curve = roads.getBezier(id)) { curves << *curve; length += curve->totalLength; } }
		if (!curves.isEmpty() && roads.getEdge(corridor.edges.front())->nodeA!=corridor.start) { const auto c=curves.front();curves.front()=CubicBezier{c.p3,c.p2,c.p1,c.p0}; }
		const double cost = constructionCost(world, curves, roads.getEdge(corridor.edges.front())->roadType);
		if (length > 240 && std::isfinite(cost) && cost > length * 4) { candidates << i; }
	}
	int rerouted=0,unbuildable=0; double savedCost = 0;
	DBG_LOG(U"[RoadAlignmentRepairStart] corridors={}"_fmt(candidates.size()));
	for (size_t candidate : candidates)
	{
		const auto& corridor=corridors[candidate];
		const RoadEdge original=*roads.getEdge(corridor.edges.front());
		const Vec3 a=roads.getNode(corridor.start)->position,b=roads.getNode(corridor.end)->position;
		Array<CubicBezier> oldCurves;
		for (const int id : corridor.edges) { if (const auto curve = roads.getBezier(id)) { oldCurves << *curve; } }
		if (!oldCurves.isEmpty() && original.nodeA!=corridor.start) { const auto c=oldCurves.front();oldCurves.front()=CubicBezier{c.p3,c.p2,c.p1,c.p0}; }
		const double limit=GenerationSettings::get().roads_maximumGeneratedViaductHeight;
		const double oldCost = maximumClearance(world,oldCurves)>limit ? Math::Inf : constructionCost(world, oldCurves, original.roadType);
		const int expansionLimit = std::isfinite(oldCost)
			? Min(12000, GenerationSettings::get().routing_generatedExpansionLimit * 3) : 60000;
		const auto alignment = find(world, a, b, original.roadType, expansionLimit, TransportMode::Road, limit);
		if (std::isfinite(oldCost) && (!alignment || alignment->cost >= oldCost * .995)) { continue; }
		if (alignment && std::isfinite(oldCost)) { savedCost += oldCost - alignment->cost; }
		if (!alignment)
		{
			// Never make the surrounding settlement float just to force an infeasible connection.
			for (int id : corridor.edges) { roads.removeEdge(id); } ++unbuildable;
			continue;
		}
		Array<int> replacements,createdNodes; int previous=corridor.start;
		for (size_t i=0;i<alignment->curves.size();++i)
		{
			const auto& curve=alignment->curves[i];
			const int next=i+1==alignment->curves.size() ? corridor.end : roads.addNode(curve.p3);
			if (i+1<alignment->curves.size()) { createdNodes << next; }
			const auto created=roads.addEdge(previous,next,curve.p1,curve.p2,original.roadType,static_cast<int>(original.lanes.size()));
			if (created)
			{
				roads.applyEdgeTemplate(*created,original); auto* edge=roads.getEdge(*created);
				edge->edgeState=original.edgeState; edge->designGrade=true;
				roads.updateEdgeElevation(*created,world); replacements << *created;
			}
			previous=next;
		}
		if (replacements.size()!=alignment->curves.size())
		{
			for (int id : replacements) { roads.removeEdge(id); }
			for (int id : createdNodes) { roads.removeNode(id); }
			for (int id : corridor.edges) { roads.removeEdge(id); }
			++unbuildable; continue;
		}
		HashSet<int> routeIds;
		for (int id : corridor.edges) { for (int routeId : roads.getEdge(id)->routeIds) { routeIds.insert(routeId); } }
		for (int routeId : routeIds)
		{
			auto* route=roads.getRoute(routeId); if (!route) { continue; }
			Array<int> revised; bool inserted=false;
			for (size_t i=0;i<route->edgeIds.size();++i)
			{
				const int id=route->edgeIds[i];
				if (!corridor.edges.contains(id)) { revised << id; continue; }
				if (inserted) { continue; }
				bool reverse=false;
				if (i>0)
				{
					const auto* before=roads.getEdge(route->edgeIds[i-1]);
					reverse=before && (before->nodeA==corridor.end || before->nodeB==corridor.end);
				}
				else
				{
					for (size_t j=i+1;j<route->edgeIds.size();++j)
					{
						if (corridor.edges.contains(route->edgeIds[j])) { continue; }
						const auto* after=roads.getEdge(route->edgeIds[j]);
						reverse=after && (after->nodeA==corridor.start || after->nodeB==corridor.start); break;
					}
				}
				for (size_t j=0;j<replacements.size();++j) { revised << replacements[reverse ? replacements.size()-1-j : j]; }
				inserted=true;
			}
			route->edgeIds=std::move(revised);
		}
		for (int id : corridor.edges) { roads.getEdge(id)->routeIds.clear(); roads.removeEdge(id); }
		++rerouted;
	}
	if (!candidates.empty()) { roads.rebuildEdgeRouteIndex(); }
	DBG_LOG(U"[RoadAlignmentRepair] candidates={} rerouted={} unbuildable={} savedCost={}"_fmt(candidates.size(),rerouted,unbuildable,savedCost));
}
