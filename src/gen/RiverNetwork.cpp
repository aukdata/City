#include "RiverNetwork.hpp"
#include <queue>

namespace
{
	/// @brief 集水格子は水源と流量の推定にだけ使い、河道の方位は連続地形から求める。
	struct Drainage
	{
		double spacing, mapWidth, mapDepth;
		int columns, rows;
		Array<double> ground, filled;
		Array<bool> ocean;
		Array<int> parent, order, flow;

		Drainage(double width, double depth, const std::function<double(double,double)>& height,
			const std::function<bool(double,double)>& marine)
			: spacing(GenerationSettings::get().rivers_catchmentGrid), mapWidth(width), mapDepth(depth),
			columns(static_cast<int>(Ceil(width/spacing))+1), rows(static_cast<int>(Ceil(depth/spacing))+1),
			ground(columns*rows), filled(columns*rows), ocean(columns*rows,false), parent(columns*rows,-1), flow(columns*rows,1)
		{
			using Entry=std::pair<double,int>;
			std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
			Array<bool> visited(columns*rows,false);
			Array<int> coast;
			for (int id=0; id<columns*rows; ++id)
			{
				const Vec2 p=position(id); ground[id]=height(p.x,p.y);
				if ((id%columns==0 || id%columns==columns-1 || id/columns==0 || id/columns==rows-1) && ground[id]<=0 && (!marine || marine(p.x,p.y)))
				{ ocean[id]=true; coast << id; }
			}
			// Marine cells are sea-level outlets; land-boundary catchments may continue beyond the map.
			for (size_t index=0; index<coast.size(); ++index)
			{
				const int id=coast[index];
				for (const Point offset : {Point{-1,0},Point{1,0},Point{0,-1},Point{0,1}})
				{
					const int x=id%columns+offset.x,z=id/columns+offset.y;
					if (x<0 || x>=columns || z<0 || z>=rows) { continue; }
					const int next=z*columns+x;
					if (!ocean[next] && ground[next]<=0 && (!marine || marine(position(next).x,position(next).y)))
					{ ocean[next]=true; coast << next; }
				}
			}
			for (int id=0; id<columns*rows; ++id)
			{
				const bool boundary=id%columns==0 || id%columns==columns-1 || id/columns==0 || id/columns==rows-1;
				if (ocean[id] || boundary)
				{ visited[id]=true; filled[id]=Max(0.0,ground[id]); pending.emplace(filled[id],id); }
			}
			while (!pending.empty())
			{
				const auto [level,id]=pending.top(); pending.pop(); order << id;
				for (int dz=-1; dz<=1; ++dz) for (int dx=-1; dx<=1; ++dx)
				{
					const int x=id%columns+dx, z=id/columns+dz;
					if ((dx==0 && dz==0) || x<0 || x>=columns || z<0 || z>=rows) { continue; }
					const int next=z*columns+x; if (visited[next]) { continue; }
					visited[next]=true; parent[next]=id; filled[next]=Max(ground[next],level+.0001*spacing);
					pending.emplace(filled[next],next);
				}
			}
			// 最初に訪問した隣接点は最急降下とは限らない。高さ差を距離で割って下流を選び直す。
			for (const int id : order)
			{
				if (parent[id]<0) { continue; }
				double steepest=0;
				for (int dz=-1; dz<=1; ++dz) for (int dx=-1; dx<=1; ++dx)
				{
					const int x=id%columns+dx,z=id/columns+dz;
					if ((dx==0 && dz==0) || x<0 || x>=columns || z<0 || z>=rows) { continue; }
					const int next=z*columns+x;
					const double grade=(filled[id]-filled[next])/(spacing*std::sqrt(static_cast<double>(dx*dx+dz*dz)));
					if (grade>steepest) { steepest=grade; parent[id]=next; }
				}
			}
			for (auto it=order.rbegin(); it!=order.rend(); ++it) { if (parent[*it]>=0) { flow[parent[*it]]+=flow[*it]; } }
		}
		Vec2 position(int id) const { return {Min((id%columns)*spacing,mapWidth),Min((id/columns)*spacing,mapDepth)}; }
		int cell(Vec2 p) const
		{
			const auto nearest=[&](double value,double extent,int count)
			{
				if (value>=((count-2)*spacing+extent)*.5) { return count-1; }
				return Clamp(static_cast<int>(Round(value/spacing)),0,count-2);
			};
			return nearest(p.y,mapDepth,rows)*columns+nearest(p.x,mapWidth,columns);
		}
	};

	struct ChannelNode { Vec2 point; int parent=-1, owner=-1; double flow=1, water=0, incision=0, alluvium=0, ground=0; };
	double cross(Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; }
}

void RiverNetwork::generate(double width, double depth, const std::function<double(double,double)>& height,
	const std::function<bool(double,double)>& marine)
{
	reaches.clear(); m_index.clear(); m_statistics={};
	const auto& settings=GenerationSettings::get();
	const Drainage drainage{width,depth,height,marine};
	const int threshold=settings.rivers_minimumCatchmentCells;
	Array<bool> upstream(drainage.flow.size(),false);
	for (const int id : drainage.order) { if (drainage.parent[id]>=0 && drainage.flow[id]>=threshold) { upstream[drainage.parent[id]]=true; } }
	Array<int> sources;
	for (const int id : drainage.order)
	{
		if (drainage.flow[id]>=threshold && !upstream[id] && !drainage.ocean[id] && drainage.ground[id]>settings.rivers_mouthAltitude) { sources << id; }
	}
	std::sort(sources.begin(),sources.end(),[&](int a,int b) { return drainage.flow[a]!=drainage.flow[b] ? drainage.flow[a]>drainage.flow[b] : a<b; });
	m_statistics.sources=static_cast<int>(sources.size());
	Array<ChannelNode> nodes;
	HashTable<Point,Array<int>> segments;
	const double step=settings.rivers_segmentLength, probe=settings.rivers_gradientProbe;
	const double indexSize=Max(32.0,step*4);
	const auto inside=[&](Vec2 p) { return p.x>=0 && p.y>=0 && p.x<=width && p.y<=depth; };
	const auto clipToBoundary=[&](Vec2 from,Vec2 to)
	{
		if (inside(to)) { return to; }
		const Vec2 move=to-from;
		const double toX=move.x>0 ? (width-from.x)/move.x : (move.x<0 ? -from.x/move.x : Math::Inf);
		const double toZ=move.y>0 ? (depth-from.y)/move.y : (move.y<0 ? -from.y/move.y : Math::Inf);
		Vec2 point=from+move*Min(toX,toZ);
		// Assign the intersected coordinate exactly; its original ground is sampled later.
		if (toX<=toZ) { point.x=move.x>0 ? width : 0; }
		if (toZ<=toX) { point.y=move.y>0 ? depth : 0; }
		return Vec2{Clamp(point.x,0.0,width),Clamp(point.y,0.0,depth)};
	};
	const auto gradient=[&](Vec2 p)
	{
		const double left=Max(0.0,p.x-probe),right=Min(width,p.x+probe),top=Max(0.0,p.y-probe),bottom=Min(depth,p.y+probe);
		return Vec2{(height(right,p.y)-height(left,p.y))/Max(.001,right-left),
			(height(p.x,bottom)-height(p.x,top))/Max(.001,bottom-top)};
	};
	const auto indexSegment=[&](int id)
	{
		const Vec2 a=nodes[id].point,b=nodes[nodes[id].parent].point;
		for (int z=static_cast<int>(Floor(Min(a.y,b.y)/indexSize)); z<=static_cast<int>(Floor(Max(a.y,b.y)/indexSize)); ++z)
			for (int x=static_cast<int>(Floor(Min(a.x,b.x)/indexSize)); x<=static_cast<int>(Floor(Max(a.x,b.x)/indexSize)); ++x) { segments[Point{x,z}] << id; }
	};
	int sinks=0,confluences=0,spillways=0,boundaryFallbackSingletons=0;
	constexpr int kTerminalExamples=8;
	const auto recordSink=[&](StringView reason,int id)
	{
		if (sinks<kTerminalExamples)
		{
			const auto& node=nodes[id];
			DBG_LOG(U"[RiverNetwork] sink reason={} id={} owner={} point=({:.17g},{:.17g}) ground={:.17g}"_fmt(
				reason,id,node.owner,node.point.x,node.point.y,height(node.point.x,node.point.y)));
		}
		++sinks;
	};
	for (const int source : sources)
	{
		const int sourceStart=static_cast<int>(nodes.size());
		int current=sourceStart;
		nodes << ChannelNode{drainage.position(source),-1,source,static_cast<double>(drainage.flow[source])};
		bool rerouted=false;
		Array<Vec2> escape;
		size_t escapeIndex=0;
		const int maximumSteps=static_cast<int>(Ceil((width+depth)*4/step));
		for (int iteration=0; iteration<maximumSteps; ++iteration)
		{
			const Vec2 p=nodes[current].point; const double level=height(p.x,p.y);
			if (drainage.ocean[drainage.cell(p)] && level<=0 && (!marine || marine(p.x,p.y))) { break; }
			const Vec2 g=gradient(p);
			// A region is a window into a larger landscape: do not route a stream back over
			// a mountain divide merely because its natural outlet lies outside this map.
			if ((p.x==0 && g.x>0) || (p.x==width && g.x<0)
				|| (p.y==0 && g.y>0) || (p.y==depth && g.y<0)) { break; }
			Vec2 next=p; bool accepted=false;
			// 通常は中点法で -grad f を積分する。谷底の折れ目だけ局所の方向微分を最小化する。
			for (double run=step; !rerouted && escape.isEmpty() && g.lengthSq()>=1e-14 && run>=settings.rivers_minimumTraceStep && !accepted; run*=.5)
			{
				const Vec2 middle=clipToBoundary(p,p-g.normalized()*(run*.5));
				const Vec2 midGradient=gradient(middle);
				if (midGradient.lengthSq()>1e-14)
				{
					next=clipToBoundary(p,p-midGradient.normalized()*run);
					if (inside(next) && height(next.x,next.y)<level-1e-8 && midGradient.normalized().dot(g.normalized())>.9) { accepted=true;break; }
				}
				// 刻みを極端に小さくする前に谷底に沿う降下方向を求め、細かな往復を避ける。
				const auto value=[&](double angle)
				{
					const Vec2 q=p+Vec2{Cos(angle),Sin(angle)}*run;
					return inside(q) ? height(q.x,q.y) : Math::Inf;
				};
				double best=Math::Inf,angle=0;
				for (int i=0;i<32;++i) { const double a=i*Math::TwoPi/32,v=value(a); if (v<best) { best=v;angle=a; } }
				double lo=angle-Math::TwoPi/32,hi=angle+Math::TwoPi/32;
				for (int i=0;i<20;++i)
				{
					const double a=(lo*2+hi)/3,b=(lo+hi*2)/3;
					if (value(a)<value(b)) { hi=b; } else { lo=a; }
				}
				const double direction=(lo+hi)*.5;
				if (value(direction)<level-1e-8)
				{
					next=p+Vec2{Cos(direction),Sin(direction)}*run;
					const Vec2 midpoint=(p+next)*.5;
					accepted=height(midpoint.x,midpoint.y)<=level;
				}
			}
			// Priority flooding supplies the lowest spillway when fine relief traps a local gradient.
			if (!accepted && escape.isEmpty())
			{
				Array<Vec2> controls{p};
				int cell=drainage.cell(p);
				for (size_t count=0; count<drainage.parent.size() && !drainage.ocean[cell] && drainage.parent[cell]>=0; ++count)
				{
					cell=drainage.parent[cell]; controls << drainage.position(cell);
				}
				const Vec2 outlet=drainage.position(cell);
				const bool boundaryOutlet=outlet.x==0 || outlet.y==0 || outlet.x==width || outlet.y==depth;
				if (boundaryOutlet && controls.size()==1 && p==outlet)
				{
					if (boundaryFallbackSingletons<kTerminalExamples)
					{
						DBG_LOG(U"[RiverNetwork] boundary fallback singleton id={} owner={} point=({:.17g},{:.17g}) gradient=({:.17g},{:.17g})"_fmt(
							current,source,p.x,p.y,g.x,g.y));
					}
					++boundaryFallbackSingletons;
					// This fallback has already reached its exact land-boundary outlet.
					break;
				}
				if (boundaryOutlet && controls.size()==1 && p!=outlet) { controls << outlet; }
				if ((!drainage.ocean[cell] && !boundaryOutlet) || controls.size()<2)
				{
					if (sinks<kTerminalExamples)
					{
						DBG_LOG(U"[RiverNetwork] escape failure controls={} cell={} ocean={} boundary={} outlet=({:.17g},{:.17g})"_fmt(
							controls.size(),cell,drainage.ocean[cell],boundaryOutlet,outlet.x,outlet.y));
					}
					recordSink(U"escape-target",current); break;
				}
				// The grid supplies drainage topology; the continuous valley floor supplies channel position.
				for (size_t i=1; i+1<controls.size(); ++i)
				{
					const Vec2 tangent=controls[i+1]-controls[i-1];
					if (tangent.lengthSq()<1e-8 || drainage.ocean[drainage.cell(controls[i])]) { continue; }
					const Vec2 normal=Vec2{-tangent.y,tangent.x}.normalized();
					Vec2 best=controls[i];
					double score=height(best.x,best.y);
					for (int side=-4; side<=4; ++side)
					{
						const double offset=side*drainage.spacing*.1;
						const Vec2 candidate=controls[i]+normal*offset;
						if (!inside(candidate)) { continue; }
						const double value=height(candidate.x,candidate.y)+offset*offset*.00005;
						if (value<score) { score=value; best=candidate; }
					}
					controls[i]=best;
				}
				for (size_t i=0; i+1<controls.size(); ++i)
				{
					const Vec2 a=controls[i==0 ? i : i-1],b=controls[i],c=controls[i+1],d=controls[Min(i+2,controls.size()-1)];
					const int pieces=Max(1,static_cast<int>(Ceil(b.distanceFrom(c)/(step*.5))));
					for (int part=1; part<=pieces; ++part)
					{
						const double t=static_cast<double>(part)/pieces,t2=t*t,t3=t2*t;
						// A spline knot keeps its authored identity instead of reevaluating t=1.
						const Vec2 point=part==pieces ? c : (b*2+(c-a)*t+(a*2-b*5+c*4-d)*t2+(-a+b*3-c*3+d)*t3)*.5;
						escape << Vec2{Clamp(point.x,0.0,width),Clamp(point.y,0.0,depth)};
					}
				}
				++spillways;
			}
			if (!escape.isEmpty())
			{
				while (escapeIndex<escape.size() && p==escape[escapeIndex]) { ++escapeIndex; }
				if (escapeIndex>=escape.size())
				{
					if (!(p.x==0 || p.y==0 || p.x==width || p.y==depth))
					{ recordSink(U"escape-exhausted",current); ++m_statistics.exhausted; }
					break;
				}
				const Vec2 toward=escape[escapeIndex]-p;
				const double remaining=toward.length();
				if (remaining<=step) { next=escape[escapeIndex++]; }
				else { next=p+toward*(step/remaining); }
				accepted=true;
			}
			if (!accepted) { recordSink(U"trace-unaccepted",current); break; }
			int joined=-1; double alongBest=2; Vec2 meeting{0,0};
			const Point cell{static_cast<int>(Floor(p.x/indexSize)),static_cast<int>(Floor(p.y/indexSize))};
			HashSet<int> candidates;
			for (int dz=-1; dz<=1; ++dz) for (int dx=-1; dx<=1; ++dx)
			{
				const auto found=segments.find(cell+Point{dx,dz});
				if (found!=segments.end()) { for (int id : found->second) { candidates.insert(id); } }
			}
			for (const int id : candidates)
			{
				if (nodes[id].parent<0 || nodes[id].parent==current) { continue; }
				const Vec2 a=nodes[id].point,b=nodes[nodes[id].parent].point,span=b-a,move=next-p;
				const double denominator=cross(move,span);
				double t=2; Vec2 at{0,0};
				if (Abs(denominator)>1e-10)
				{
					const double u=cross(a-p,move)/denominator,v=cross(a-p,span)/denominator;
					if (u>=0 && u<=1 && v>=0 && v<=1) { t=v; at=p+move*v; }
				}
				if (t>1 && nodes[id].owner!=source)
				{
					const double u=Clamp((next-a).dot(span)/Max(1e-12,span.lengthSq()),0.0,1.0);
					at=a+span*u;
					if (at.distanceFrom(next)<=settings.rivers_confluenceSnap && (at-p).dot(move)>0 && height(at.x,at.y)<=level) { t=1; }
				}
				if (t<alongBest || (t==alongBest && id<joined)) { alongBest=t; joined=id; meeting=at; }
			}
			if (joined>=0 && alongBest<=1)
			{
				// 近接合流の短い接続も再検査する。微小な吸着で隣の流路を横切らせない。
				for (int pass=0; pass<4; ++pass)
				{
					const Vec2 move=meeting-p; double first=1; int crossing=-1; Vec2 point=meeting;
					for (const int id : candidates)
					{
						if (nodes[id].parent<0 || nodes[id].parent==current) { continue; }
						const Vec2 a=nodes[id].point,b=nodes[nodes[id].parent].point,span=b-a;
						const double denominator=cross(move,span); if (Abs(denominator)<1e-10) { continue; }
						const double t=cross(a-p,span)/denominator,u=cross(a-p,move)/denominator;
						if (t>=0 && t<first-1e-8 && u>=0 && u<=1) { crossing=id;first=t;point=p+move*t; }
					}
					if (crossing<0) { break; }
					joined=crossing;meeting=point;
				}
				if (nodes[joined].owner==source)
				{
					++m_statistics.selfIntersections;
					if (rerouted) { recordSink(U"rerouted-self-intersection",current); break; }
					// The local descent doubled back into its own upstream course. Rebuild this
					// source from its drainage route so the eventual outlet remains acyclic.
					nodes.resize(sourceStart);
					segments.clear();
					for (int id=0; id<sourceStart; ++id) { if (nodes[id].parent>=0) { indexSegment(id); } }
					current=sourceStart;
					nodes << ChannelNode{drainage.position(source),-1,source,static_cast<double>(drainage.flow[source])};
					escape.clear(); escapeIndex=0; rerouted=true; iteration=-1;
					continue;
				}
				int target=nodes[joined].parent;
				if (meeting.distanceFromSq(nodes[joined].point)<1e-8) { target=joined; }
				else if (meeting.distanceFromSq(nodes[target].point)>=1e-8)
				{
					const int split=static_cast<int>(nodes.size());
					nodes << ChannelNode{meeting,target,nodes[joined].owner,nodes[joined].flow};
					nodes[joined].parent=split; target=split; indexSegment(split);
				}
				nodes[current].parent=target; indexSegment(current); ++confluences; break;
			}
			const int nextId=static_cast<int>(nodes.size());
			nodes << ChannelNode{next,-1,source,static_cast<double>(drainage.flow[drainage.cell(next)])};
			nodes[current].parent=nextId; indexSegment(current); current=nextId;
		}
	}
	// Independently traced outlets can have one exact boundary position but separate node IDs.
	HashTable<Vec2,int> boundaryTerminals;
	int duplicateBoundaryTerminals=0,canonicalizedBoundaryLinks=0;
	for (size_t id=0; id<nodes.size(); ++id)
	{
		const auto& node=nodes[id];
		if (node.parent>=0 || !(node.point.x==0 || node.point.y==0 || node.point.x==width || node.point.y==depth)) { continue; }
		const Vec2 point{node.point.x==0 ? 0 : node.point.x,node.point.y==0 ? 0 : node.point.y};
		const auto found=boundaryTerminals.find(point);
		if (found==boundaryTerminals.end()) { boundaryTerminals.emplace(point,static_cast<int>(id)); continue; }
		if (duplicateBoundaryTerminals<kTerminalExamples)
		{
			DBG_LOG(U"[RiverNetwork] duplicate boundary terminal id={} canonical={} owner={} canonicalOwner={} point=({:.17g},{:.17g})"_fmt(
				id,found->second,node.owner,nodes[found->second].owner,point.x,point.y));
		}
		++duplicateBoundaryTerminals;
	}
	// Redirect only incoming terminal edges before solving shared flow, water and sediment.
	// A canonical terminal has no downstream edge, so this identity correction cannot add a cycle.
	for (auto& node : nodes)
	{
		if (node.parent<0 || nodes[node.parent].parent>=0) { continue; }
		const auto& terminal=nodes[node.parent];
		const Vec2 point{terminal.point.x==0 ? 0 : terminal.point.x,terminal.point.y==0 ? 0 : terminal.point.y};
		const auto found=boundaryTerminals.find(point);
		if (found==boundaryTerminals.end() || found->second==node.parent) { continue; }
		nodes[found->second].flow=Max(nodes[found->second].flow,terminal.flow);
		node.parent=found->second;
		++canonicalizedBoundaryLinks;
	}
	// 合流後の流量と水位はグラフ全体で確定し、支流を跨いでも逆流・川幅の縮小を作らない。
	Array<int> children(nodes.size(),0),queue;
	for (const auto& node : nodes) { if (node.parent>=0) { ++children[node.parent]; } }
	Array<double> incoming(nodes.size(),0);
	for (size_t id=0; id<nodes.size(); ++id)
	{
		nodes[id].ground=height(nodes[id].point.x,nodes[id].point.y);
		nodes[id].water=Max(0.0,nodes[id].ground-settings.rivers_valleyWaterDepth);
		if (children[id]==0) { queue << static_cast<int>(id); }
	}
	for (size_t index=0; index<queue.size(); ++index)
	{
		const int id=queue[index],next=nodes[id].parent;
		nodes[id].flow=Max(nodes[id].flow,incoming[id]);
		if (next<0) { continue; }
		incoming[next]+=nodes[id].flow;
		nodes[next].water=Min(nodes[next].water,Max(0.0,nodes[id].water-settings.rivers_minimumWaterGrade*nodes[id].point.distanceFrom(nodes[next].point)));
		if (--children[next]==0) { queue << next; }
	}
	// Grade tributary approaches upstream from each shared confluence. A low trunk must not
	// create an instantaneous vertical water step at the final, very short joining segment.
	// Steep natural headwaters retain the original terrain grade instead of being forced flat.
	constexpr double kHeadwaterApproachGrade=.06, kTrunkApproachGrade=.008, kNaturalGradeAllowance=.002;
	for (auto it=queue.rbegin();it!=queue.rend();++it)
	{
		auto& node=nodes[*it];
		if (node.parent<0) { continue; }
		const auto& next=nodes[node.parent];
		const double run=node.point.distanceFrom(next.point);
		if (run<1e-8) { continue; }
		const double channelWidth=settings.rivers_widthBase+Sqrt(node.flow)*settings.rivers_widthFlowScale;
		const double trunk=Clamp((channelWidth-30)/70.0,0.0,1.0);
		const double naturalGrade=Max(0.0,(node.ground-next.ground)/run);
		const double maximumGrade=Max(Math::Lerp(kHeadwaterApproachGrade,kTrunkApproachGrade,trunk),naturalGrade+kNaturalGradeAllowance);
		node.water=Min(node.water,next.water+maximumGrade*run);
	}
	// Sediment load travels through the same directed confluence graph as water.
	// High stream power cuts the bed; falling capacity deposits the carried load.
	Array<double> sediment(nodes.size(),0);
	Array<int> principalChild(nodes.size(),-1);
	for (const int id : queue)
	{
		const int next=nodes[id].parent;
		if (next<0) { continue; }
		const double run=nodes[id].point.distanceFrom(nodes[next].point);
		if (run<1e-6) { continue; }
		const double grade=Max(settings.rivers_minimumWaterGrade,(nodes[id].water-nodes[next].water)/run);
		const double capacity=settings.rivers_sedimentCapacity*std::sqrt(nodes[id].flow)*std::sqrt(grade);
		nodes[id].incision=Min(4.0,Max(0.0,capacity-sediment[id])*settings.rivers_erosionRate);
		nodes[id].alluvium=Min(2.0,Max(0.0,sediment[id]-capacity)*settings.rivers_depositionRate);
		sediment[next]+=sediment[id]+nodes[id].incision-nodes[id].alluvium;
		if (principalChild[next]<0 || nodes[id].flow>nodes[principalChild[next]].flow) { principalChild[next]=id; }
	}
	const auto halfWidth=[&](double flow)
	{
		return Clamp(settings.rivers_widthBase+std::sqrt(flow)*settings.rivers_widthFlowScale,settings.rivers_minimumHalfWidth,settings.rivers_maximumHalfWidth);
	};
	double length=0,maximumOmittedLength=0;
	int omittedDistinct=0,omittedCoincident=0;
	constexpr int kOmittedLinkExamples=8;
	for (const int id : queue)
	{
		const auto& node=nodes[id]; if (node.parent<0) { continue; }
		const auto& next=nodes[node.parent];
		if (node.point.distanceFromSq(next.point)<1e-8)
		{
			if (node.point==next.point) { ++omittedCoincident; }
			else
			{
				const double run=node.point.distanceFrom(next.point);
				maximumOmittedLength=Max(maximumOmittedLength,run);
				if (omittedDistinct<kOmittedLinkExamples)
				{
					DBG_LOG(U"[RiverNetwork] omitted distinct id={} parent={} owner={} nextOwner={} start=({:.17g},{:.17g}) end=({:.17g},{:.17g}) run={:.17g} water=({:.17g},{:.17g})"_fmt(
						id,node.parent,node.owner,next.owner,node.point.x,node.point.y,next.point.x,next.point.y,run,node.water,next.water));
				}
				++omittedDistinct;
			}
			continue;
		}
		Reach reach; reach.start={node.point.x,node.water,node.point.y}; reach.end={next.point.x,next.water,next.point.y};
		reach.halfWidth=halfWidth(node.flow); reach.endHalfWidth=halfWidth(next.flow); reach.catchment=node.flow*drainage.spacing*drainage.spacing;
		reach.incision=(node.incision+next.incision)*.5;
		reach.alluvium=(node.alluvium+next.alluvium)*.5;
		// A deep channel needs a broad valley shoulder rather than a cliff at the carve boundary.
		const Vec2 flow=next.point-node.point;
		const Vec2 across=Vec2{-flow.y,flow.x}.normalized();
		const Vec2 middle=(node.point+next.point)*.5;
		const double bankProbeDistance=Max(reach.halfWidth,reach.endHalfWidth)+settings.rivers_carveExtent;
		const double bankHigh=Max(height(middle.x,middle.y),Max(height(middle.x+across.x*bankProbeDistance,middle.y+across.y*bankProbeDistance),
			height(middle.x-across.x*bankProbeDistance,middle.y-across.y*bankProbeDistance)));
		const double relief=Max(0.0,bankHigh-(node.water+next.water)*.5);
		reach.bankExtent=reach.halfWidth<80 ? Clamp(settings.rivers_carveExtent+(relief-15)*2.2,settings.rivers_carveExtent,250.0)
			: settings.rivers_carveExtent;
		const int mainChild=principalChild[id];
		if (mainChild>=0 && reach.alluvium>.2 && id%4==0)
		{
			const Vec2 entering=node.point-nodes[mainChild].point,leaving=next.point-node.point;
			if (entering.lengthSq()>1 && leaving.lengthSq()>1)
			{
				const double bend=cross(entering.normalized(),leaving.normalized());
				if (Abs(bend)>.08) { reach.barSide=bend>0 ? -.7 : .7; }
			}
		}
		const Vec2 lower{Min(node.point.x,next.point.x),Min(node.point.y,next.point.y)},upper{Max(node.point.x,next.point.x),Max(node.point.y,next.point.y)};
		reach.bounds=RectF{lower,upper-lower}.stretched(Max(reach.halfWidth,reach.endHalfWidth)+reach.bankExtent+8);
		length+=node.point.distanceFrom(next.point);
		const int reachId=static_cast<int>(reaches.size()); reaches << reach;
		for (int z=static_cast<int>(Floor(reach.bounds.y/512)); z<=static_cast<int>(Floor(reach.bounds.br().y/512)); ++z)
			for (int x=static_cast<int>(Floor(reach.bounds.x/512)); x<=static_cast<int>(Floor(reach.bounds.br().x/512)); ++x) { m_index[key(x,z)] << reachId; }
	}
	DBG_LOG(U"[RiverNetwork] omittedDistinct={} omittedCoincident={} maximumOmittedLength={:.17g}"_fmt(omittedDistinct,omittedCoincident,maximumOmittedLength));
	DBG_LOG(U"[RiverNetwork] duplicateBoundaryTerminals={} canonicalizedBoundaryLinks={} boundaryFallbackSingletons={}"_fmt(
		duplicateBoundaryTerminals,canonicalizedBoundaryLinks,boundaryFallbackSingletons));
	m_statistics.spillways=spillways;
	m_statistics.unresolved=sinks;
	DBG_LOG(U"[RiverNetwork] reaches={} lengthKm={:.1f} confluences={} spillways={} sinks={} self={} exhausted={}"_fmt(reaches.size(),length/1000,confluences,spillways,sinks,m_statistics.selfIntersections,m_statistics.exhausted));
}
