#pragma once
#include "../gen/GenerationSettings.hpp"
#include <Siv3D.hpp>

/// @brief 高度帯と地形に沿った森林境界。確率場を連続にし、木の候補自体は固定する。
namespace VegetationProfile
{
	inline double blend(double value,double from,double to)
	{
		const double t=Clamp((value-from)/Max(.001,to-from),0.0,1.0);return t*t*(3-2*t);
	}
	struct Altitude { double trees,alpine,snow,treeScale; };
	inline Altitude at(double height)
	{
		const auto& config=GenerationSettings::get();
		const double transition=blend(height,config.vegetation_maximumForestAltitude,config.vegetation_treeLine);
		const double snow=blend(height,config.vegetation_snowStart,config.vegetation_snowFull);
		return {1-transition,transition*(1-snow)*config.vegetation_alpineDensity,snow,Math::Lerp(1.0,.32,transition)};
	}
	inline bool habitat(double height,double slope)
	{
		const auto& config=GenerationSettings::get();
		return height>=config.vegetation_minimumForestAltitude && height<=config.vegetation_treeLine && slope<=config.vegetation_maximumForestSlope
			&& !(height<config.vegetation_flatClearingAltitude && slope<config.vegetation_flatClearingSlope);
	}
	/// @brief グローバル格子上の境界距離を2パスで求める。500mの余白を含め、隣のチャンクと連続する。
	class BoundaryField
	{
		Vec2 m_origin;
		double m_step,m_fade;
		Grid<double> m_distance;
	public:
		BoundaryField(RectF area,const std::function<bool(Vec2)>& suitable)
			: m_step(GenerationSettings::get().vegetation_boundarySampleSpacing)
			, m_fade(GenerationSettings::get().vegetation_boundaryFadeDistance)
		{
			m_origin={Floor((area.x-m_fade-m_step)/m_step)*m_step,Floor((area.y-m_fade-m_step)/m_step)*m_step};
			const int width=static_cast<int>(Ceil((area.rightX()+m_fade+m_step-m_origin.x)/m_step))+1;
			const int height=static_cast<int>(Ceil((area.bottomY()+m_fade+m_step-m_origin.y)/m_step))+1;
			m_distance=Grid<double>(width,height,m_fade+m_step*2);
			for(int y=0;y<height;++y) { for(int x=0;x<width;++x) { if(!suitable(m_origin+Vec2{x*m_step,y*m_step})) { m_distance[y][x]=0; } } }
			for(int pass=0;pass<2;++pass)
			{
				const int step=pass==0 ? 1 : -1;
				for(int yi=0;yi<height;++yi) { for(int xi=0;xi<width;++xi)
				{
					const int x=pass==0 ? xi : width-1-xi,y=pass==0 ? yi : height-1-yi;
					for(const Point offset:{Point{-step,0},Point{0,-step},Point{-step,-step},Point{step,-step}})
					{
						const int nx=x+offset.x,ny=y+offset.y;if(!InRange(nx,0,width-1) || !InRange(ny,0,height-1)) { continue; }
						m_distance[y][x]=Min(m_distance[y][x],m_distance[ny][nx]+m_step*(offset.x!=0 && offset.y!=0 ? Sqrt(2.0) : 1));
					}
				} }
			}
		}
		[[nodiscard]] double density(Vec2 point) const
		{
			const Vec2 local=(point-m_origin)/m_step;const int x=Clamp(static_cast<int>(Floor(local.x)),0,static_cast<int>(m_distance.width())-2),y=Clamp(static_cast<int>(Floor(local.y)),0,static_cast<int>(m_distance.height())-2);
			const double fx=Clamp(local.x-x,0.0,1.0),fy=Clamp(local.y-y,0.0,1.0);
			const double distance=Math::Lerp(Math::Lerp(m_distance[y][x],m_distance[y][x+1],fx),Math::Lerp(m_distance[y+1][x],m_distance[y+1][x+1],fx),fy);
			return blend(distance,0,m_fade);
		}
	};
}
