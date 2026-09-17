#pragma once
#include <Siv3D.hpp>

/// @brief 表示範囲ごとの標高・陰影画像。拡大時は地形を再サンプルして細部を保つ。
class MapTerrainLayer
{
public:
	RectF bounds;
	DynamicTexture texture;
	double metersPerPixel=0;
	void invalidate() { m_valid=false; }
	bool update(RectF visible,Size pixels,const std::function<float(Vec2)>& sample)
	{
		const double requested=visible.w*1.5/pixels.x;
		if (m_valid && texture.size()==pixels && bounds.contains(visible.tl()) && bounds.contains(visible.br())
			&& metersPerPixel<=requested*1.1 && metersPerPixel>=requested*.6) { return false; }
		const Vec2 cell=visible.size/8;
		const Vec2 center{Math::Floor(visible.center().x/cell.x)*cell.x,Math::Floor(visible.center().y/cell.y)*cell.y};
		bounds={center-visible.size*.75,visible.size*1.5};
		metersPerPixel=bounds.w/pixels.x;
		const Vec2 step{bounds.w/pixels.x,bounds.h/pixels.y};
		Grid<float> heights(static_cast<size_t>(pixels.x+2),static_cast<size_t>(pixels.y+2));
		for (int y=0;y<pixels.y+2;++y) for (int x=0;x<pixels.x+2;++x)
		{
			const Vec2 world=bounds.pos+Vec2{(x-.5)*step.x,(y-.5)*step.y};
			heights[y][x]=sample({Clamp(world.x,0.0,65535.99),Clamp(world.y,0.0,65535.99)});
		}
		Image image{pixels,Color{0,0,0,0}};
		for (int y=0;y<pixels.y;++y) for (int x=0;x<pixels.x;++x)
		{
			// 境界外の画素にもクランプした地形色を持たせ、補間による黒縁を防ぐ。
			const float height=heights[y+1][x+1];
			const double dx=(heights[y+1][x+2]-heights[y+1][x])/(2*step.x);
			const double dz=(heights[y+2][x+1]-heights[y][x+1])/(2*step.y);
			const double light=height<0 ? 1 : Clamp(.9+(-dx*.55-dz*.45)/Math::Sqrt(1+dx*dx+dz*dz),.57,1.18);
			const ColorF color=height<0 ? ColorF{.37,.62,.75}.lerp(ColorF{.18,.4,.61},Clamp(-height/70.0,0.0,1.0))
				: height<180 ? ColorF{.69,.77,.56}.lerp(ColorF{.51,.64,.43},Clamp(height/180.0,0.0,1.0))
				: ColorF{.51,.64,.43}.lerp(ColorF{.82,.81,.74},Clamp((height-180)/1100.0,0.0,1.0));
			image[y][x]=ColorF{color.r*light,color.g*light,color.b*light};
		}
		if (texture.size()!=pixels) { texture=DynamicTexture{image}; }
		else
		{
			// GPU更新は即時実行なので、旧画像を参照する2Dコマンドを先に完了する。
			const ScopedCustomShader2D restoreShaders{VertexShader{},PixelShader{}};
			Graphics2D::Flush();
			if (!texture.fill(image)) { texture=DynamicTexture{image}; }
		}
		m_valid=true; return true;
	}
private:
	bool m_valid=false;
};
