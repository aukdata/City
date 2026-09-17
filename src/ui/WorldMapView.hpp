#pragma once
#include "PlainLabel.hpp"
#include "KeyboardActions.hpp"
#include "MapStroke.hpp"
#include <Siv3D.hpp>

/// @brief 地図の座標変換・ズーム・右クリックメニュー。地形は背景、道路は拡大率に応じた線で描く。
class WorldMapView
{
public:
	struct Stroke { Array<Vec2> points; RectF bounds; double width = 5; int category = 0; bool tunnel = false; };
	struct Label { Vec2 position; String text; bool station = false; double minimumZoom=1; String reading; };
	Array<Stroke> streets,rivers;
	struct Border { Vec2 a,b; int level=0; };
	Array<Border> boundaries;
	std::function<String(Vec2)> addressAt;
	Array<Label> labels;
	bool visible = false;
	Vec2 center{32768, 32768};
	double zoom = 1;
	Optional<Vec2> contextWorld;
	Vec2 contextScreen;
	static constexpr double kWorldSize = 65536;

	RectF body(Size size) const { return {0, 56, size.x, Max(1, size.y-56)}; }
	double scale(Size size) const { return Min(size.x, Max(1, size.y-56)) / kWorldSize * zoom; }
	Vec2 toScreen(Vec2 world, Size size) const { return body(size).center() + Vec2{center.x-world.x,world.y-center.y}*scale(size); }
	Vec2 toWorld(Vec2 pixel, Size size) const { return center + Vec2{body(size).center().x-pixel.x,pixel.y-body(size).center().y}/scale(size); }
	void open(Vec2 focus)
	{
		if (!m_hasView) { center=focus; zoom=4; m_hasView=true; }
		visible=true; contextWorld.reset();
	}
	/// @brief Invalidate after editing the map geometry, labels or source terrain.
	void invalidateCartography() { m_cartographyDirty=true; }
	[[nodiscard]] uint64 cartographyBuilds() const { return m_cartographyBuilds; }
	void close() { visible = false; contextWorld.reset(); }
	/// @brief Return to the current camera while preserving the selected map scale.
	void recenter(Vec2 focus)
	{
		center = focus;
		clampCenter();
		contextWorld.reset();
	}
	void zoomAt(Vec2 pixel, double factor, Size size)
	{
		const Vec2 anchor = toWorld(pixel, size);
		zoom = Clamp(zoom*factor, 1.0, 512.0);
		center += anchor-toWorld(pixel, size);
		clampCenter();
	}
	void pan(Vec2 pixelDelta, Size size) { center += Vec2{pixelDelta.x,-pixelDelta.y}/scale(size); clampCenter(); }

	/// @brief 地図上の方向に一定の画面速度で移動する。斜め移動も同じ速度。
	void panKeyboard(Vec2 direction, double seconds, Size size,bool fast=false)
	{
		if (direction.lengthSq()<.01) { return; }
		pan(-direction.normalized()*420*(fast ? 3 : 1)*Clamp(seconds,0.0,.1),size);
		contextWorld.reset();
	}
	void showContext(Vec2 pixel, Size size)
	{
		const Vec2 world = toWorld(pixel, size);
		if (world.x<0 || world.y<0 || world.x>kWorldSize || world.y>kWorldSize) { contextWorld.reset(); return; }
		contextWorld = world;
		contextScreen = {Clamp(pixel.x, 8.0, Max(8.0, size.x-232.0)), Clamp(pixel.y, 64.0, Max(64.0, size.y-60.0))};
	}
	RectF menuBounds() const { return {contextScreen, 224, 48}; }
	Optional<Vec2> jumpFromMenu(Vec2 pixel)
	{
		if (!contextWorld || !menuBounds().contains(pixel)) { return none; }
		const auto target = contextWorld; close(); return target;
	}
	Optional<Vec2> update(Size size, Optional<Vec2> camera = none)
	{
		if (!visible) { return none; }
		if (camera && GameInput::down(KeyHome)) { recenter(*camera); }
		if (GameInput::down(KeyEscape) || GameInput::down(KeyM) || RectF{size.x-118, 10, 106, 36}.leftClicked()) { close(); return none; }
		panKeyboard({static_cast<double>(GameInput::pressed(KeyD))-GameInput::pressed(KeyA),static_cast<double>(GameInput::pressed(KeyS))-GameInput::pressed(KeyW)},Scene::DeltaTime(),size,GameInput::pressed(KeyControl));
		const Vec2 cursor = Cursor::PosF();
		if (contextWorld && MouseL.down())
		{
			const auto target = jumpFromMenu(cursor);
			contextWorld.reset(); return target;
		}
		if (RectF{size.x-60, 78, 42, 42}.leftClicked()) { zoomAt(body(size).center(), 2, size); }
		else if (RectF{size.x-60, 124, 42, 42}.leftClicked()) { zoomAt(body(size).center(), .5, size); }
		else if (body(size).contains(cursor))
		{
			if (Mouse::Wheel()!=0) { zoomAt(cursor, std::pow(1.3, -Mouse::Wheel()), size); contextWorld.reset(); }
			if (MouseR.down()) { showContext(cursor, size); }
			if (MouseL.pressed() && !MouseL.down() && !contextWorld) { pan(Vec2{Cursor::Delta()}, size); }
		}
		return none;
	}
	void draw(Size size, const Texture& terrain, const Font& font, Vec2 camera,RectF terrainWorld={0,0,kWorldSize,kWorldSize}) const
	{
		// Cached geography never contains font-atlas commands. Labels stay in the final UI pass.
		const bool rebuild=m_cartographyDirty || m_cacheSize!=size || m_cacheCenter!=center || m_cacheZoom!=zoom;
		const ScopedCustomShader2D standard{VertexShader{},PixelShader{}};
		if (rebuild) { Graphics2D::Flush(); }
		if (rebuild)
		{
			if (m_cacheSize!=size) { m_cartography=RenderTexture{size,TextureFormat::R8G8B8A8_Unorm}; }
			{
				const ScopedRenderTarget2D target{m_cartography.clear(ColorF{.91,.93,.88})};
				drawCartography(size,terrain,terrainWorld);
			}
			m_cacheSize=size; m_cacheCenter=center; m_cacheZoom=zoom;
			m_cartographyDirty=false; ++m_cartographyBuilds;
		}
		m_cartography.draw();
		drawLabels(size,font);
		if (body(size).contains(toScreen(camera,size)))
		{
			Circle{toScreen(camera, size), 7}.draw(ColorF{.1,.45,.94}).drawFrame(2, Palette::White);
		}
		RectF{0, 0, size.x, 56}.draw(ColorF{1});
		font(U"街の地図").draw(18, 14, ColorF{.13,.22,.29});
		font(U"WASD:移動 / ホイール:拡大 / Home:現在地 / 右クリック:ジャンプ").draw(130, 17, ColorF{.35});
		RectF{size.x-118, 10, 106, 36}.draw(ColorF{.91,.94,.97});
		font(U"閉じる  Esc").drawAt(size.x-65, 27, ColorF{.22});
		for (int index=0; index<2; ++index)
		{
			RectF{size.x-60, 78+46*index, 42, 42}.draw(Palette::White).drawFrame(1, ColorF{.65});
			font(index==0 ? U"＋" : U"−").drawAt(size.x-39, 99+46*index, ColorF{.2});
		}
		const double meters = std::pow(10.0, std::floor(std::log10(130.0/scale(size))));
		const double pixels = meters*scale(size);
		RectF{16, size.y-52, Max(142.0,pixels+24), 40}.draw(ColorF{1,.9});
		Line{Vec2{28.0,size.y-20.0},Vec2{28.0+pixels,size.y-20.0}}.draw(3,ColorF{.3});
		font(meters>=1000 ? U"{} km"_fmt(meters/1000) : U"{} m"_fmt(meters)).draw(28,size.y-48,ColorF{.2});
		if (addressAt)
		{
			const String address=addressAt(toWorld(Cursor::PosF(),size));
			const auto text=font(address); const double width=text.region().w+24;
			PlainLabel::draw(font,address,font.fontSize(),{size.x-width-4,size.y-42},ColorF{.25},ColorF{1,.85});
		}
		if (contextWorld)
		{
			menuBounds().movedBy(2,3).draw(ColorF{0,.16});
			menuBounds().draw(Palette::White).drawFrame(1, ColorF{.7});
			font(U"ここへジャンプ").draw(contextScreen+Vec2{18,13}, ColorF{.13,.32,.51});
		}
	}
private:
	bool m_hasView=false;
	mutable RenderTexture m_cartography;
	mutable Size m_cacheSize{0,0};
	mutable Vec2 m_cacheCenter;
	mutable double m_cacheZoom=0;
	mutable bool m_cartographyDirty=true;
	mutable uint64 m_cartographyBuilds=0;
	void drawCartography(Size size,const Texture& terrain,RectF terrainWorld) const
	{
		RectF{0, 0, size.x, size.y}.draw(ColorF{.91, .93, .88});
		const Rect previous=Graphics2D::GetScissorRect();
		Graphics2D::SetScissorRect(Rect{0, 56, size.x, Max(1, size.y-56)});
		{
			const ScopedRenderStates2D clip{RasterizerState{FillMode::Solid, CullMode::Off, true}};
			const Vec2 upper = toWorld({0, 56}, size), lower = toWorld({static_cast<double>(size.x), static_cast<double>(size.y)}, size);
			const RectF visibleWorld{Vec2{Min(upper.x,lower.x),Min(upper.y,lower.y)},Vec2{Abs(lower.x-upper.x),Abs(lower.y-upper.y)}};
			if (!terrain.isEmpty() && terrainWorld.w>0 && terrainWorld.h>0)
			{
				const Vec2 lo{Max(0.0,Max(terrainWorld.x,visibleWorld.x)),Max(0.0,Max(terrainWorld.y,visibleWorld.y))};
				const Vec2 hi{Min(kWorldSize,Min(terrainWorld.rightX(),visibleWorld.rightX())),Min(kWorldSize,Min(terrainWorld.bottomY(),visibleWorld.bottomY()))};
				if (hi.x>lo.x && hi.y>lo.y)
				{
					const RectF clipped{lo,hi-lo};
					const RectF uv{(lo.x-terrainWorld.x)/terrainWorld.w,(lo.y-terrainWorld.y)/terrainWorld.h,clipped.w/terrainWorld.w,clipped.h/terrainWorld.h};
					Quad{toScreen(clipped.tl(),size),toScreen(clipped.tr(),size),toScreen(clipped.br(),size),toScreen(clipped.bl(),size)}(terrain.uv(uv)).draw();
				}
			}
			for (const auto& river : rivers)
			{
				if (!river.bounds.intersects(visibleWorld)) { continue; }
				if (river.points.size()<2) { continue; }
				MapStroke::draw(toScreen(river.points.front(),size),toScreen(river.points.back(),size),body(size),Max(1.2,river.width*scale(size)),ColorF{.38,.67,.82});
			}
			for (const auto& border : boundaries)
			{
				if (zoom<(border.level==0 ? 1 : border.level==1 ? 8 : 32)) { continue; }
				if (!Line{border.a,border.b}.intersects(visibleWorld)) { continue; }
				MapStroke::draw(toScreen(border.a,size),toScreen(border.b,size),body(size),border.level==0 ? 2.2 : 1.2,ColorF{.48,.28,.53,.7},true);
			}
			for (const int pass : {0, 1})
			{
				for (const auto& road : streets)
				{
					if (!road.bounds.intersects(visibleWorld)) { continue; }
					const double width = Clamp(road.width*scale(size), road.category==0 ? 1.5 : 1.8, 24.0);
					const ColorF color = pass==0 ? ColorF{.68,.68,.64} : (road.category==2 ? ColorF{.35,.4,.46} : (road.category==1 ? ColorF{1,.83,.39} : ColorF{1}));
					const size_t stride=static_cast<size_t>(Max(1.0,std::floor(1.5/(30*scale(size)))));
					double phase=0;
					for (size_t index=1; index<road.points.size(); index+=stride)
					{
						const Vec2 a=toScreen(road.points[index-1],size),b=toScreen(road.points[Min(index+stride-1,road.points.size()-1)],size);
						MapStroke::draw(a,b,body(size),width+(pass==0 ? 2 : 0),color,road.tunnel,phase);
						phase+=a.distanceFrom(b);
					}
				}
			}


		}
		Graphics2D::SetScissorRect(previous);
	}
	void drawLabels(Size size,const Font& font) const
	{
		const Rect previous=Graphics2D::GetScissorRect();
		Graphics2D::SetScissorRect(Rect{0,56,size.x,Max(1,size.y-56)});
		{
			const ScopedRenderStates2D clip{RasterizerState{FillMode::Solid,CullMode::Back,true}};
			Array<RectF> occupied;
			for (const auto& label : labels)
			{
				if (zoom<label.minimumZoom) { continue; }
				const Vec2 point = toScreen(label.position, size);
				if (!body(size).stretched(-24).contains(point)) { continue; }
				const double width=Max(font(label.text).region().w,font(label.reading).region(11).w)+12;
				const RectF bounds{point+Vec2{6,-10},width,label.reading.isEmpty() ? 26.0 : 40.0};
				bool overlaps=false;
				for (const auto& used : occupied) { if (used.intersects(bounds)) { overlaps=true; break; } }
				if (overlaps) { continue; }
				occupied << bounds;
				if (label.station) { RectF{point-Vec2{4,4},8,8}.draw(ColorF{.18,.36,.55}); }
				PlainLabel::draw(font,label.text,font.fontSize(),bounds.pos+Vec2{6,2},ColorF{.17,.24,.29},ColorF{1,.85});
				if (!label.reading.isEmpty()) { PlainLabel::draw(font,label.reading,11,bounds.pos+Vec2{6,24},ColorF{.3,.38,.41},ColorF{1,.85}); }
			}
		}
		Graphics2D::SetScissorRect(previous);
	}
	void clampCenter() { center.x=Clamp(center.x,0.0,kWorldSize); center.y=Clamp(center.y,0.0,kWorldSize); }
};
