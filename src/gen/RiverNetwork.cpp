#include "RiverNetwork.hpp"
#include <queue>

namespace
{
	/// @brief 集水格子は水源と流量の推定にだけ使い、河道の方位は連続地形から求める。
	struct Drainage
	{
		double spacing;
		int columns, rows;
		Array<double> ground, filled;
		Array<int> parent, order, flow;

		Drainage(double width, double depth, const std::function<double(double,double)>& height)
			: spacing(GenerationSettings::get().rivers_catchmentGrid),
			columns(static_cast<int>(width/spacing)+1), rows(static_cast<int>(depth/spacing)+1),
			ground(columns*rows), filled(columns*rows), parent(columns*rows,-1), flow(columns*rows,1)
		{
			using Entry=std::pair<double,int>;
			std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
			Array<bool> visited(columns*rows,false);
			for (int id=0; id<columns*rows; ++id)
			{
				const Vec2 p=position(id); ground[id]=height(p.x,p.y);
				if (id%columns==0 || id%columns==columns-1 || id/columns==0 || id/columns==rows-1 || ground[id]<=0)
				{
					visited[id]=true; filled[id]=Max(0.0,ground[id]); pending.emplace(filled[id],id);
				}
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
		Vec2 position(int id) const { return {(id%columns)*spacing,(id/columns)*spacing}; }
		int cell(Vec2 p) const { return Clamp(static_cast<int>(Round(p.y/spacing)),0,rows-1)*columns+Clamp(static_cast<int>(Round(p.x/spacing)),0,columns-1); }
	};

	struct ChannelNode { Vec2 point; int parent=-1, owner=-1; double flow=1, water=0; };
	double cross(Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; }
}

void RiverNetwork::generate(double width, double depth, const std::function<double(double,double)>& height)
{
	reaches.clear(); m_index.clear();
	const auto& settings=GenerationSettings::get();
	const Drainage drainage{width,depth,height};
	const int threshold=settings.rivers_minimumCatchmentCells;
	Array<bool> upstream(drainage.flow.size(),false);
	for (const int id : drainage.order) { if (drainage.parent[id]>=0 && drainage.flow[id]>=threshold) { upstream[drainage.parent[id]]=true; } }
	Array<int> sources;
	for (const int id : drainage.order)
	{
		if (drainage.flow[id]>=threshold && !upstream[id] && drainage.ground[id]>settings.rivers_mouthAltitude) { sources << id; }
	}
	std::sort(sources.begin(),sources.end(),[&](int a,int b) { return drainage.flow[a]!=drainage.flow[b] ? drainage.flow[a]>drainage.flow[b] : a<b; });
	Array<ChannelNode> nodes;
	HashTable<Point,Array<int>> segments;
	const double step=settings.rivers_segmentLength, probe=settings.rivers_gradientProbe;
	const double indexSize=Max(32.0,step*4);
	const auto inside=[&](Vec2 p) { return p.x>=0 && p.y>=0 && p.x<=width && p.y<=depth; };
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
	int sinks=0,confluences=0;
	for (const int source : sources)
	{
		int current=static_cast<int>(nodes.size());
		nodes << ChannelNode{drainage.position(source),-1,source,static_cast<double>(drainage.flow[source])};
		const int maximumSteps=static_cast<int>(Ceil((width+depth)*4/step));
		for (int iteration=0; iteration<maximumSteps; ++iteration)
		{
			const Vec2 p=nodes[current].point; const double level=height(p.x,p.y);
			if (level<=settings.rivers_mouthAltitude || p.x<step || p.y<step || p.x>width-step || p.y>depth-step) { break; }
			const Vec2 g=gradient(p);
			if (g.lengthSq()<1e-14) { ++sinks; break; }
			Vec2 next=p; bool accepted=false;
			// 通常は中点法で -grad f を積分する。谷底の折れ目だけ局所の方向微分を最小化する。
			for (double run=step; run>=settings.rivers_minimumTraceStep && !accepted; run*=.5)
			{
				const Vec2 middle=p-g.normalized()*(run*.5);
				if (!inside(middle)) { continue; }
				const Vec2 midGradient=gradient(middle);
				if (midGradient.lengthSq()>1e-14)
				{
					next=p-midGradient.normalized()*run;
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
			// 閉じた窪地は水の終端。丘を越える直線を追加して海へ強制接続しない。
			if (!accepted) { ++sinks; break; }
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
				if (nodes[joined].owner==source) { ++sinks; break; }
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
	// 合流後の流量と水位はグラフ全体で確定し、支流を跨いでも逆流・川幅の縮小を作らない。
	Array<int> children(nodes.size(),0),queue;
	for (const auto& node : nodes) { if (node.parent>=0) { ++children[node.parent]; } }
	Array<double> incoming(nodes.size(),0);
	for (size_t id=0; id<nodes.size(); ++id)
	{
		nodes[id].water=Max(0.0,height(nodes[id].point.x,nodes[id].point.y)-settings.rivers_valleyWaterDepth);
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
	const auto halfWidth=[&](double flow)
	{
		return Clamp(settings.rivers_widthBase+std::sqrt(flow)*settings.rivers_widthFlowScale,settings.rivers_minimumHalfWidth,settings.rivers_maximumHalfWidth);
	};
	double length=0;
	for (const int id : queue)
	{
		const auto& node=nodes[id]; if (node.parent<0) { continue; }
		const auto& next=nodes[node.parent]; if (node.point.distanceFromSq(next.point)<1e-8) { continue; }
		Reach reach; reach.start={node.point.x,node.water,node.point.y}; reach.end={next.point.x,next.water,next.point.y};
		reach.halfWidth=halfWidth(node.flow); reach.endHalfWidth=halfWidth(next.flow); reach.catchment=node.flow*drainage.spacing*drainage.spacing;
		const Vec2 lower{Min(node.point.x,next.point.x),Min(node.point.y,next.point.y)},upper{Max(node.point.x,next.point.x),Max(node.point.y,next.point.y)};
		reach.bounds=RectF{lower,upper-lower}.stretched(Max(reach.halfWidth,reach.endHalfWidth)+80);
		length+=node.point.distanceFrom(next.point);
		const int reachId=static_cast<int>(reaches.size()); reaches << reach;
		for (int z=static_cast<int>(Floor(reach.bounds.y/512)); z<=static_cast<int>(Floor(reach.bounds.br().y/512)); ++z)
			for (int x=static_cast<int>(Floor(reach.bounds.x/512)); x<=static_cast<int>(Floor(reach.bounds.br().x/512)); ++x) { m_index[key(x,z)] << reachId; }
	}
	DBG_LOG(U"[RiverNetwork] reaches={} lengthKm={:.1f} confluences={} sinks={}"_fmt(reaches.size(),length/1000,confluences,sinks));
}
