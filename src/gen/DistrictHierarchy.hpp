#pragma once
#include "MapGenerator.hpp"
#include "SettlementNames.hpp"
#include <queue>

/// @brief 市町村 → 町・大字 → 丁目・小字。河川と峠の通過費用を用いて連続した区域を分割する。
class DistrictHierarchy
{
public:
	struct Area { int id=-1,parent=-1,level=0; String name; Vec2 center; bool urban=false; String reading; };
	struct Boundary { Vec2 a,b; int level=0; };
	Array<Area> areas;
	Array<Boundary> boundaries;
	static constexpr int kColumns=static_cast<int>(WORLD_SIZE)/128;
	static constexpr double kCell=128;

	void generate(const World& world,const Array<MapGenerator::Settlement>& towns)
	{
		areas.clear(); boundaries.clear(); for (auto& layer : m_owner) { layer.assign(kColumns*kColumns,-1); }
		if (towns.isEmpty()) { return; }
		Array<float> ground(kColumns*kColumns); Array<float> river(kColumns*kColumns);
		for (int cell=0;cell<kColumns*kColumns;++cell)
		{
			const Vec2 p=point(cell); ground[cell]=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.y));
			const auto channel=world.rivers().nearest(p); river[cell]=channel.distance<channel.halfWidth+60 ? 1.0f : 0.0f;
		}
		const auto add=[&](int level,int parent,String name,Vec2 center,bool urban,String reading=U"")
		{
			const int id=static_cast<int>(areas.size()); areas << Area{id,parent,level,std::move(name),center,urban,std::move(reading)}; return id;
		};
		for (const auto& town : towns)
		{
			add(0,-1,SettlementNames::name(town),town.center,town.kind!=MapGenerator::SettlementKind::RuralSettlement,SettlementNames::reading(town));
		}
		partition(0,ground,river);
		for (const auto& town : towns)
		{
			const bool urban=town.kind!=MapGenerator::SettlementKind::RuralSettlement;
			const int parent=at(town.center,0); if (parent<0) { continue; }
			add(1,parent,urban ? town.name+U"本町" : U"大字"+town.name,town.center,urban,town.reading+(urban ? U" Honmachi" : U""));
			if (town.kind==MapGenerator::SettlementKind::RegionalCity)
			{
				const std::array<String,4> names{U"東町",U"西町",U"南町",U"北町"};
				for (int i=0;i<4;++i)
				{
					const Vec2 p=town.center+(i<2 ? town.gridAxisX : town.gridAxisZ)*(i%2==0 ? 1.0 : -1.0)*Max(300.0,static_cast<double>(town.radius)*.45);
					if (at(p,0)==parent) { add(1,parent,town.name+names[i],p,true,town.reading+std::array<String,4>{U" Higashimachi",U" Nishimachi",U" Minamimachi",U" Kitamachi"}[i]); }
				}
			}
		}
		partition(1,ground,river);
		const int count=static_cast<int>(areas.size());
		const std::array<String,4> numbers{U"一丁目",U"二丁目",U"三丁目",U"四丁目"};
		const std::array<Vec2,4> directions{Vec2{1,0},Vec2{0,1},Vec2{-1,0},Vec2{0,-1}};
		for (int id=0;id<count;++id)
		{
			const auto area=areas[id]; if (area.level!=1) { continue; }
			for (int i=0;i<4;++i)
			{
				const Vec2 p=area.center+directions[i]*(area.urban ? 192 : 320);
				if (at(p,1)!=id) { continue; }
				const auto channel=world.rivers().nearest(p);
				const String name=area.urban ? numbers[i] : (channel.distance<500 ? U"字川端" : ground[cellAt(p)]>65 ? U"字山際" : U"字原")+std::array<String,4>{U"東",U"南",U"西",U"北"}[i];
				const String reading=area.urban ? U"{}-chome"_fmt(i+1) : (channel.distance<500 ? U"Kawabata" : ground[cellAt(p)]>65 ? U"Yamagiwa" : U"Hara")+std::array<String,4>{U" Higashi",U" Minami",U" Nishi",U" Kita"}[i];
				add(2,id,name,p,area.urban,reading);
			}
			bool child=false; for (const auto& candidate : areas) { child|=candidate.parent==id; }
			if (!child) { add(2,id,area.urban ? U"一丁目" : U"字中村",area.center,area.urban,area.urban ? U"1-chome" : U"Nakamura"); }
		}
		partition(2,ground,river);
		// Shared cell edges are emitted once. River-adjacent vertices follow the actual channel center.
		const auto fit=[&](Vec2 p)
		{
			const auto channel=world.rivers().nearest(p);
			return channel.distance<80 ? channel.center : p;
		};
		for (int z=0;z<kColumns;++z) for (int x=0;x<kColumns;++x)
		{
			const int cell=z*kColumns+x; if (ground[cell]<0) { continue; }
			for (int direction=0;direction<2;++direction)
			{
				if ((direction==0 && x+1==kColumns) || (direction==1 && z+1==kColumns)) { continue; }
				const int next=cell+(direction==0 ? 1 : kColumns); if (ground[next]<0) { continue; }
				int level=0; while (level<3 && m_owner[level][cell]==m_owner[level][next]) { ++level; } if (level==3) { continue; }
				const Vec2 a=direction==0 ? Vec2{(x+1)*kCell,z*kCell} : Vec2{x*kCell,(z+1)*kCell};
				const Vec2 b=a+(direction==0 ? Vec2{0,kCell} : Vec2{kCell,0});
				const Vec2 start=fit(a),end=fit(b); if (start.distanceFromSq(end)>1) { boundaries << Boundary{start,end,level}; }
			}
		}
		int missing=0; for (int cell=0;cell<kColumns*kColumns;++cell) { missing+=m_owner[2][cell]<0; }
		DBG_LOG(U"[DistrictHierarchy] areas={} boundaries={} uncoveredCells={}"_fmt(areas.size(),boundaries.size(),missing));
	}
	int at(Vec2 p,int level) const { if (level<0 || level>2 || m_owner[level].isEmpty()) { return -1; } return m_owner[level][cellAt(p)]; }
	String address(Vec2 p) const
	{
		String result; for (int level=0;level<3;++level) { const int id=at(p,level); if (id>=0) { result+=areas[id].name; } } return result;
	}
private:
	std::array<Array<int>,3> m_owner;
	static int cellAt(Vec2 p) { return Clamp(static_cast<int>(p.y/kCell),0,kColumns-1)*kColumns+Clamp(static_cast<int>(p.x/kCell),0,kColumns-1); }
	static Vec2 point(int cell) { return {(cell%kColumns+.5)*kCell,(cell/kColumns+.5)*kCell}; }
	void partition(int level,const Array<float>& ground,const Array<float>& river)
	{
		using Entry=std::pair<double,int>;
		std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
		Array<double> distances(kColumns*kColumns,Math::Inf);
		for (auto& area : areas)
		{
			if (area.level!=level) { continue; }
			int cell=cellAt(area.center);
			if (m_owner[level][cell]>=0) { continue; }
			m_owner[level][cell]=area.id; distances[cell]=0; pending.emplace(0,cell);
		}
		while (!pending.empty())
		{
			const auto [distance,cell]=pending.top(); pending.pop(); if (distance>distances[cell]) { continue; }
			const int owner=m_owner[level][cell],x=cell%kColumns,z=cell/kColumns;
			for (const Point direction : {Point{1,0},Point{-1,0},Point{0,1},Point{0,-1}})
			{
				const int xx=x+direction.x,zz=z+direction.y; if (xx<0 || xx>=kColumns || zz<0 || zz>=kColumns) { continue; }
				const int next=zz*kColumns+xx;
				if (level>0 && m_owner[level-1][next]!=areas[owner].parent) { continue; }
				const double cost=1+Min(45.0,Square(static_cast<double>(ground[cell]-ground[next]))*.025)+(river[cell]!=river[next] ? 65 : 0);
				const double candidate=distance+cost;
				if (candidate<distances[next]) { distances[next]=candidate; m_owner[level][next]=owner; pending.emplace(candidate,next); }
			}
		}
	}
};
