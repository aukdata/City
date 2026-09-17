#pragma once
#include "GenerationSettings.hpp"
#include <Siv3D.hpp>
#include <functional>
#include "../debug/DebugLog.hpp"

/// @brief 流域の集水・合流・下流への河床低下を共有する河川網。単位はm。
class RiverNetwork
{
public:
	struct Reach { Vec3 start,end; double halfWidth=20; double endHalfWidth=20; double barSide=0; double catchment=0; RectF bounds; };
	struct Sample { Vec2 center; double distance=1e30; double surface=0; double halfWidth=0; int reach=-1; };
	Array<Reach> reaches;
	/// @brief 集水から水源を選び、連続地形の -grad f を追跡して河道と合流を形成する。
	void generate(double width, double depth, const std::function<double(double, double)>& height);
	Sample nearest(Vec2 point) const
	{
		Sample result;
		const auto found=m_index.find(key(static_cast<int>(point.x/512),static_cast<int>(point.y/512))); if (found==m_index.end()) { return result; }
		for (const int id : found->second)
		{
			const auto& reach=reaches[id]; const Vec2 a{reach.start.x,reach.start.z},b{reach.end.x,reach.end.z},delta=b-a;
			const double t=Clamp((point-a).dot(delta)/Max(.01,delta.lengthSq()),0.0,1.0); const Vec2 center=a+delta*t;
			const double distance=point.distanceFrom(center);
			const double width=Math::Lerp(reach.halfWidth,reach.endHalfWidth,t);
			if (distance-width<result.distance-result.halfWidth) { result={center,distance,Math::Lerp(reach.start.y,reach.end.y,t),width,id}; }
		}
		return result;
	}
	/// @brief 樹冠・根元とも水面と河岸の余裕幅に侵入させない。
	bool vegetationAllowed(Vec2 point,double ground,double radius=10) const
	{
		const auto sample=nearest(point);
		return ground>GenerationSettings::get().rivers_vegetationMinimumAltitude && (sample.reach<0 || sample.distance>sample.halfWidth+radius+GenerationSettings::get().rivers_vegetationBankMargin);
	}
	/// @brief 非同期景観生成へ渡す読み取り専用の局所水系。
	RiverNetwork subset(const RectF& bounds) const
	{
		RiverNetwork result;
		for (const auto& reach : reaches)
		{
			if (!reach.bounds.intersects(bounds)) { continue; }
			const int index=static_cast<int>(result.reaches.size()); result.reaches<<reach;
			for (int z=static_cast<int>(Floor(reach.bounds.y/512));z<=static_cast<int>(Floor(reach.bounds.br().y/512));++z)
				for (int x=static_cast<int>(Floor(reach.bounds.x/512));x<=static_cast<int>(Floor(reach.bounds.br().x/512));++x) { result.m_index[key(x,z)]<<index; }
		}
		return result;
	}
	double waterLevel(Vec2 point) const { const auto sample=nearest(point); return sample.distance<=sample.halfWidth+GenerationSettings::get().rivers_waterEdgeMargin ? sample.surface : 0; }
	double carveHeight(Vec2 point,double original) const
	{
		const auto sample=nearest(point); if (sample.reach<0 || sample.distance>sample.halfWidth+GenerationSettings::get().rivers_carveExtent) { return original; }
		const double bank=Max(0.0,sample.distance-sample.halfWidth);
		const double target=sample.surface-GenerationSettings::get().rivers_bedDepth+bank*GenerationSettings::get().rivers_bankSlope;
		const double blend=Clamp((sample.distance-sample.halfWidth-GenerationSettings::get().rivers_bankBlendStart)/GenerationSettings::get().rivers_bankBlendWidth,0.0,1.0);
		return Math::Lerp(Min(original,target),original,blend*blend*(3-2*blend));
	}
private:
	static int64 key(int x,int z) { return static_cast<int64>(x)*0x100000000LL+static_cast<uint32>(z); }
	HashTable<int64,Array<int>> m_index;
};
