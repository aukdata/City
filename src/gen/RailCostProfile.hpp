#pragma once
#include <Siv3D.hpp>
#include "../debug/DebugLog.hpp"

/// @brief 縦断を地上・切土・盛土・高架の相対費用で選ぶ。費用は通貨ではなく比較用の単位。
namespace RailCostProfile
{
	struct Sample { Vec2 position; double ground=0,minimum=0,maximum=0,roadClearance=-1e9,underpass=1e9; };
	struct Result { Array<double> heights; double cost=Math::Inf; bool feasible=false; };
	inline double unitCost(double difference)
	{
		if (difference < -5) { return 28.0+Abs(difference)*.18; }
		if (difference<0) { return 1.0+Square(difference)*1.8; }
		if (difference<=3) { return 1.0+Square(difference)*.55; }
		return 5.0+difference*.8+Square(difference)*.12;
	}
	inline Result solve(const Array<Sample>& samples,double start,double end,double grade=.018)
	{
		Result result; if (samples.size()<2) { return result; }
		constexpr double step=.5;
		double low=Min(start,end),high=Max(start,end);
		for (const auto& sample : samples) { low=Min(low,sample.minimum); high=Max(high,sample.maximum); }
		const int origin=static_cast<int>(std::floor(low/step)),count=static_cast<int>(std::ceil(high/step))-origin+1;
		if (count>5000) { return result; }
		Array<double> previous(count,Math::Inf),next(count,Math::Inf);
		Array<int> parents(samples.size()*count,-1);
		const int first=static_cast<int>(std::round(start/step))-origin,last=static_cast<int>(std::round(end/step))-origin;
		previous[first]=0;
		for (size_t i=1;i<samples.size();++i)
		{
			next.fill(Math::Inf); const double run=samples[i].position.distanceFrom(samples[i-1].position);
			const int delta=static_cast<int>(std::ceil(run*grade/step))+1;
			int lower=Max(0,static_cast<int>(std::ceil(samples[i].minimum/step))-origin),upper=Min(count-1,static_cast<int>(std::floor(samples[i].maximum/step))-origin);
			if (i+1==samples.size()) { lower=upper=last; }
			for (int level=lower;level<=upper;++level)
			{
				if (i+1==samples.size() && level!=last) { continue; }
				const double elevation=i+1==samples.size() ? end : (origin+level)*step;
				if (elevation<samples[i].roadClearance && elevation>samples[i].underpass) { continue; }
				for (int from=Max(0,level-delta);from<=Min(count-1,level+delta);++from)
				{
					if (!std::isfinite(previous[from])) { continue; }
					const double previousHeight=i==1 ? start : (origin+from)*step;
					const double slope=(elevation-previousHeight)/Max(.01,run);
					if (Abs(slope)>grade+1e-8) { continue; }
					const double cost=previous[from]+run*(unitCost(elevation-samples[i].ground)+Square(slope/grade)*.5);
					if (cost<next[level]) { next[level]=cost; parents[i*count+level]=from; }
				}
			}
			previous.swap(next);
			bool any=false; for (const double value : previous) { any|=std::isfinite(value); }
			if (!any) { DBG_LOG(U"[RailProfileBlocked] sample={} run={} lower={} upper={} road={} start={} end={}"_fmt(i,run,samples[i].minimum,samples[i].maximum,samples[i].roadClearance,start,end)); return result; }
		}
		if (!std::isfinite(previous[last])) { return result; }
		result.feasible=true; result.cost=previous[last]; result.heights.resize(samples.size());
		int level=last;
		for (size_t i=samples.size();i-->0;) { result.heights[i]=(origin+level)*step; if (i>0) { level=parents[i*count+level]; } }
		result.heights.front()=start; result.heights.back()=end;
		// Remove quantisation ripples inside the feasible envelope without changing terminal levels.
		for (int pass=0;pass<40;++pass)
		{
			for (size_t i=1;i+1<samples.size();++i)
			{
				const double before=samples[i].position.distanceFrom(samples[i-1].position),after=samples[i].position.distanceFrom(samples[i+1].position);
				const double minimum=Max(samples[i].minimum,Max(result.heights[i-1]-before*grade,result.heights[i+1]-after*grade));
				const double maximum=Min(samples[i].maximum,Min(result.heights[i-1]+before*grade,result.heights[i+1]+after*grade));
				if (minimum<=maximum) { const double proposed=Clamp(result.heights[i]*.6+(result.heights[i-1]*after+result.heights[i+1]*before)/Max(1.0,before+after)*.4,minimum,maximum); if (proposed>=samples[i].roadClearance || proposed<=samples[i].underpass) { result.heights[i]=proposed; } }
			}
		}
		return result;
	}
}
