#include "RiverNetwork.hpp"
#include <queue>

namespace
{
	/// @brief 接続点を共有する河道の平面曲線。水位決定とメッシュ化で同じ形を使う。
	struct ChannelCurve
	{
		Vec2 first, controlA, controlB, last;
		Vec2 at(double fraction) const
		{
			const double reverse = 1 - fraction;
			return first * (reverse * reverse * reverse) + controlA * (3 * reverse * reverse * fraction)
				+ controlB * (3 * reverse * fraction * fraction) + last * (fraction * fraction * fraction);
		}
	};

	ChannelCurve channelCurve(int id, const Array<Vec2>& centers, const Array<int>& parent, const Array<int>& mainChild)
	{
		const int next = parent[id];
		const Vec2 first = centers[id], last = centers[next];
		const double run = first.distanceFrom(last);
		const Vec2 incoming = mainChild[id] >= 0 ? (last - centers[mainChild[id]]) * .5 : last - first;
		const Vec2 outgoing = parent[next] >= 0 ? (centers[parent[next]] - first) * .5 : last - first;
		const auto tangent = [&](Vec2 direction)
		{
			return direction.lengthSq() > .01 ? direction.normalized() * Min(run, direction.length()) : last - first;
		};
		return {first, first + tangent(incoming) / 3, last - tangent(outgoing) / 3, last};
	}

	double channelHalfWidth(int flow)
	{
		const auto& settings = GenerationSettings::get();
		return Clamp(settings.rivers_widthBase + std::sqrt(static_cast<double>(flow)) * settings.rivers_widthFlowScale,
			settings.rivers_minimumHalfWidth, settings.rivers_maximumHalfWidth);
	}

	/// @brief 合流先から位相を伝え、川幅に応じた蛇行と実際の谷底の位置を両立する。
	void bendChannels(Array<Vec2>& centers, const Array<int>& parent, const Array<int>& mainChild,
		const Array<int>& flow, const Array<int>& order, const Array<double>& ground,
		double width, double depth, const std::function<double(double, double)>& height, uint64 seed)
	{
		const auto& settings = GenerationSettings::get();
		const int count = static_cast<int>(centers.size()), threshold = settings.rivers_minimumCatchmentCells;
		const double spacing = settings.rivers_catchmentGrid;
		const Array<Vec2> original = centers;
		Array<double> phase(count, 0), junctionDistance(count, 1e30);
		Array<int> branches(count, 0);
		HashTable<Point, Array<int>> index;
		for (int id = 0; id < count; ++id)
		{
			const int next = parent[id];
			if (next < 0 || flow[id] < threshold) { continue; }
			++branches[next];
			const Vec2 lower{Min(original[id].x, original[next].x), Min(original[id].y, original[next].y)};
			const Vec2 upper{Max(original[id].x, original[next].x), Max(original[id].y, original[next].y)};
			for (int z = static_cast<int>(Floor(lower.y / spacing)) - 1; z <= static_cast<int>(Floor(upper.y / spacing)) + 1; ++z)
			{
				for (int x = static_cast<int>(Floor(lower.x / spacing)) - 1; x <= static_cast<int>(Floor(upper.x / spacing)) + 1; ++x) { index[Point{x, z}] << id; }
			}
		}
		// 合流・河口付近は共有点へ穏やかに収束させ、支流ごとの不連続を作らない。
		for (const int id : order)
		{
			const int next = parent[id];
			if (next < 0)
			{
				uint64 hash = seed ^ (static_cast<uint64>(id) * 0x9e3779b97f4a7c15ULL);
				hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
				phase[id] = Math::TwoPi * static_cast<double>(hash >> 11) / 9007199254740992.0;
			}
			else
			{
				const double run = original[id].distanceFrom(original[next]);
				const double wavelength = Max(settings.rivers_meanderMinimumWavelength,
					2 * channelHalfWidth(flow[id]) * settings.rivers_meanderWavelengthWidths);
				phase[id] = phase[next] + Math::TwoPi * run / wavelength;
				junctionDistance[id] = junctionDistance[next] + run;
			}
			if (branches[id] > 1 || next < 0 || ground[id] <= settings.rivers_smoothingMinimumAltitude) { junctionDistance[id] = 0; }
		}
		for (auto it = order.rbegin(); it != order.rend(); ++it)
		{
			const int id = *it, next = parent[id];
			if (next >= 0 && flow[id] >= threshold)
			{
				junctionDistance[next] = Min(junctionDistance[next], junctionDistance[id] + original[id].distanceFrom(original[next]));
			}
		}
		for (int id = 0; id < count; ++id)
		{
			const int next = parent[id];
			if (next < 0 || flow[id] < threshold || junctionDistance[id] < 1) { continue; }
			const Vec2 origin = original[id];
			Vec2 direction = original[next] - (mainChild[id] >= 0 ? original[mainChild[id]] : origin);
			if (direction.lengthSq() < 1) { continue; }
			direction = direction.normalized(); const Vec2 side{-direction.y, direction.x};
			double radius = Min(spacing * .45, 2 * channelHalfWidth(flow[id]) * settings.rivers_meanderAmplitudeWidths);
			const Point cell{static_cast<int>(Floor(origin.x / spacing)), static_cast<int>(Floor(origin.y / spacing))};
			const auto found = index.find(cell);
			if (found != index.end())
			{
				for (const int other : found->second)
				{
					const int after = parent[other];
					// 同じ流路の前後2区間は連続する曲がりなので、別の川との離隔には数えない。
					if (other == id || other == next || other == parent[next]
						|| after == id || after == next || after == mainChild[id]) { continue; }
					const Vec2 delta = original[after] - original[other];
					const double t = Clamp((origin - original[other]).dot(delta) / Max(1.0, delta.lengthSq()), 0.0, 1.0);
					const double clearance = origin.distanceFrom(original[other] + delta * t);
					radius = Min(radius, Max(0.0, clearance - channelHalfWidth(flow[id]) - channelHalfWidth(flow[other])) * .25);
				}
			}
			if (radius < 1) { continue; }
			const double grade = Abs(ground[id] - ground[next]) / Max(1.0, origin.distanceFrom(original[next]));
			const double slopeRatio = grade / settings.rivers_meanderGradeScale;
			const double taper = Min(1.0, junctionDistance[id] / (2 * spacing));
			const double secondary = settings.rivers_meanderSecondaryShare;
			const double target = radius * ((1 - secondary) * Sin(phase[id]) + secondary * Sin(phase[id] * .6180339887498948 + 1))
				/ (1 + slopeRatio * slopeRatio) * taper * taper * (3 - 2 * taper);
			const int samples = settings.rivers_courseSearchSamples;
			Array<std::pair<double, double>> candidates;
			double lowest = height(origin.x, origin.y);
			for (int sample = -samples; sample <= samples; ++sample)
			{
				const double offset = radius * sample / samples;
				const Vec2 point = origin + side * offset;
				if (point.x < 0 || point.y < 0 || point.x > width || point.y > depth) { continue; }
				const double level = height(point.x, point.y); lowest = Min(lowest, level);
				candidates.emplace_back(offset, level);
			}
			double best = Math::Inf;
			for (const auto& [offset, level] : candidates)
			{
				const double rise = (level - lowest) / settings.rivers_courseBankRiseLimit;
				if (rise > 1) { continue; }
				const double deviation = (offset - target) / radius;
				const double cost = deviation * deviation + rise * rise;
				if (cost < best) { best = cost; centers[id] = origin + side * offset; }
			}
		}
		// 中心点が低地にあっても、その間の曲線が谷壁へ膨らむ場合は移動量を縮める。
		// 基準河道から追加で高い土地へ出る分を制限し、周辺4点で接線の連続を保つ。
		for (int iteration = 0; iteration < 6; ++iteration)
		{
			Array<int> limited;
			for (int id = 0; id < count; ++id)
			{
				const int next = parent[id];
				if (next < 0 || flow[id] < threshold) { continue; }
				const auto before = channelCurve(id, original, parent, mainChild);
				const auto after = channelCurve(id, centers, parent, mainChild);
				if (before.first == after.first && before.last == after.last && before.controlA == after.controlA && before.controlB == after.controlB) { continue; }
				bool outside = false;
				for (int sample = 1; sample < 8; ++sample)
				{
					const Vec2 a = before.at(sample / 8.0), b = after.at(sample / 8.0);
					if (b.x < 0 || b.y < 0 || b.x > width || b.y > depth || height(b.x, b.y) > height(a.x, a.y) + settings.rivers_courseBankRiseLimit)
					{
						outside = true; break;
					}
				}
				if (outside) { limited << id << next; if (mainChild[id] >= 0) { limited << mainChild[id]; } if (parent[next] >= 0) { limited << parent[next]; } }
			}
			if (limited.isEmpty()) { break; }
			limited.sort_and_unique();
			for (const int id : limited) { centers[id] = original[id].lerp(centers[id], iteration < 5 ? .5 : 0.0); }
		}
	}
}

void RiverNetwork::generate(double width, double depth, const std::function<double(double, double)>& height, uint64 seed)
{
	reaches.clear(); m_index.clear();
	const double spacing=GenerationSettings::get().rivers_catchmentGrid;
	const int columns=static_cast<int>(width/spacing)+1,rows=static_cast<int>(depth/spacing)+1,count=columns*rows;
	Array<double> ground(count),filled(count),water(count); Array<int> parent(count,-1),order,flow(count,1),mainChild(count,-1);
	Array<bool> visited(count,false);
	using Entry=std::pair<double,int>;
	std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
	const auto position=[&](int id) { return Vec2{(id%columns)*spacing,(id/columns)*spacing}; };
	for (int id=0;id<count;++id)
	{
		const Vec2 point=position(id); ground[id]=height(point.x,point.y); water[id]=Max(0.0,ground[id]-GenerationSettings::get().rivers_initialWaterDepth);
		const bool border=id%columns==0 || id%columns==columns-1 || id/columns==0 || id/columns==rows-1;
		if (border || ground[id]<=0) { visited[id]=true; filled[id]=Max(0.0,ground[id]); pending.emplace(filled[id],id); }
	}
	// くぼみの流出先を共有する排水木を作り、下流へ循環させない。
	while (!pending.empty())
	{
		const auto [level,id]=pending.top(); pending.pop(); order << id;
		for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
		{
			if (dx==0 && dz==0) { continue; }
			const int x=id%columns+dx,z=id/columns+dz; if (x<0 || x>=columns || z<0 || z>=rows) { continue; }
			const int next=z*columns+x; if (visited[next]) { continue; }
			visited[next]=true; parent[next]=id; filled[next]=Max(ground[next],level+.0001*spacing); pending.emplace(filled[next],next);
		}
	}
	for (auto it=order.rbegin();it!=order.rend();++it)
	{
		const int id=*it,next=parent[id]; if (next<0) { continue; }
		flow[next]+=flow[id]; if (mainChild[next]<0 || flow[id]>flow[mainChild[next]]) { mainChild[next]=id; }
	}
	const int threshold=GenerationSettings::get().rivers_minimumCatchmentCells;
	Array<Vec2> centers(count);
	for (int id=0;id<count;++id)
	{
		centers[id]=position(id);
		if (flow[id]>=threshold && parent[id]>=0 && mainChild[id]>=0 && ground[id]>GenerationSettings::get().rivers_smoothingMinimumAltitude)
		{
			centers[id]=position(id)*.5+(position(parent[id])+position(mainChild[id]))*.25;
		}
	}
	// 蛇行を先に確定し、その曲線の谷底から水位を決める。
	bendChannels(centers, parent, mainChild, flow, order, ground, width, depth, height, seed);
	for (int id=0;id<count;++id)
	{
		const int next=parent[id]; if (next<0 || flow[id]<threshold) { continue; }
		double low=1e9;
		const ChannelCurve curve = channelCurve(id, centers, parent, mainChild);
		const int samples = Max(16, static_cast<int>(Ceil(centers[id].distanceFrom(centers[next]) / GenerationSettings::get().rivers_segmentLength)));
		for (int sample = 0; sample <= samples; ++sample) { const Vec2 point = curve.at(static_cast<double>(sample) / samples); low = Min(low, height(point.x, point.y)); }
		water[id]=Min(water[id],Max(0.0,low-GenerationSettings::get().rivers_valleyWaterDepth)); water[next]=Min(water[next],Max(0.0,low-GenerationSettings::get().rivers_valleyWaterDepth));
	}
	for (auto it=order.rbegin();it!=order.rend();++it)
	{
		const int id=*it,next=parent[id]; if (next<0 || flow[id]<threshold) { continue; }
		water[next]=Min(water[next],Max(0.0,water[id]-GenerationSettings::get().rivers_minimumWaterGrade*centers[id].distanceFrom(centers[next])));
	}
	double length=0,maxCut=0; int confluences=0;
	for (int id=0;id<count;++id)
	{
		const int next=parent[id]; if (next<0 || flow[id]<threshold || ground[id]<GenerationSettings::get().rivers_mouthAltitude) { continue; }
		const Vec2 first=centers[id],last=centers[next];
		const double run=first.distanceFrom(last);
		if (run<1) { continue; }
		const ChannelCurve curve = channelCurve(id, centers, parent, mainChild);
		const double widthA = channelHalfWidth(flow[id]), widthB = channelHalfWidth(flow[next]);
		const int sections=Max(2,static_cast<int>(std::ceil(run/GenerationSettings::get().rivers_segmentLength)));
		const auto pointAt=[&](double fraction)
		{
			const Vec2 p = curve.at(fraction);
			return Vec3{p.x,Math::Lerp(water[id],water[next],fraction),p.y};
		};
		for (int section=0;section<sections;++section)
		{
			Reach reach; reach.start=pointAt(static_cast<double>(section)/sections); reach.end=pointAt(static_cast<double>(section+1)/sections);
			reach.halfWidth=Math::Lerp(widthA,widthB,static_cast<double>(section)/sections);
			reach.endHalfWidth=Math::Lerp(widthA,widthB,static_cast<double>(section+1)/sections);
			reach.catchment=flow[id]*spacing*spacing;
			const Vec2 lower{Min(reach.start.x,reach.end.x),Min(reach.start.z,reach.end.z)},upper{Max(reach.start.x,reach.end.x),Max(reach.start.z,reach.end.z)};
			reach.bounds=RectF{lower,upper-lower}.stretched(Max(reach.halfWidth,reach.endHalfWidth)+80);
			length += Vec2{reach.end.x - reach.start.x, reach.end.z - reach.start.z}.length();
			const int index=static_cast<int>(reaches.size()); reaches<<reach;
			for (int z=static_cast<int>(Floor(reach.bounds.y/512));z<=static_cast<int>(Floor(reach.bounds.br().y/512));++z)
				for (int x=static_cast<int>(Floor(reach.bounds.x/512));x<=static_cast<int>(Floor(reach.bounds.br().x/512));++x) { m_index[key(x,z)]<<index; }
		}
		maxCut = Max(maxCut, height(first.x, first.y) - water[id]);
		confluences+=mainChild[next]>=0 && mainChild[next]!=id && flow[mainChild[next]]>=threshold;
	}
	DBG_LOG(U"[RiverNetwork] reaches={} lengthKm={:.1f} confluences={} maximumIncision={:.1f}"_fmt(reaches.size(),length/1000,confluences,maxCut));
}
