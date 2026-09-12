#pragma once
#include <Siv3D.hpp>

/// @brief 地図の座標変換・ズーム・右クリックメニュー。地形は背景、道路は拡大率に応じた線で描く。
class WorldMapView
{
public:
	struct Stroke { Array<Vec2> points; RectF bounds; double width = 5; int category = 0; };
	struct Label { Vec2 position; String text; bool station = false; };
	Array<Stroke> streets;
	Array<Label> labels;
	bool visible = false;
	Vec2 center{32768, 32768};
	double zoom = 1;
	Optional<Vec2> contextWorld;
	Vec2 contextScreen;
	static constexpr double kWorldSize = 65536;

	RectF body(Size size) const { return {0, 56, size.x, Max(1, size.y-56)}; }
	double scale(Size size) const { return Min(size.x, Max(1, size.y-56)) / kWorldSize * zoom; }
	Vec2 toScreen(Vec2 world, Size size) const { return body(size).center() + (world-center)*scale(size); }
	Vec2 toWorld(Vec2 pixel, Size size) const { return center + (pixel-body(size).center())/scale(size); }
	void open(Vec2 focus) { visible = true; center = focus; zoom = 4; contextWorld.reset(); }
	void close() { visible = false; contextWorld.reset(); }
	void zoomAt(Vec2 pixel, double factor, Size size)
	{
		const Vec2 anchor = toWorld(pixel, size);
		zoom = Clamp(zoom*factor, 1.0, 512.0);
		center += anchor-toWorld(pixel, size);
		clampCenter();
	}
	void pan(Vec2 pixelDelta, Size size) { center -= pixelDelta/scale(size); clampCenter(); }
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
	Optional<Vec2> update(Size size)
	{
		if (!visible) { return none; }
		if (KeyEscape.down() || KeyM.down() || RectF{size.x-118, 10, 106, 36}.leftClicked()) { close(); return none; }
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
	void draw(Size size, const Texture& terrain, const Font& font, Vec2 camera) const
	{
		RectF{0, 0, size.x, size.y}.draw(ColorF{.91, .93, .88});
		Graphics2D::SetScissorRect(Rect{0, 56, size.x, Max(1, size.y-56)});
		{
			const ScopedRenderStates2D clip{RasterizerState{FillMode::Solid, CullMode::Back, true}};
			if (!terrain.isEmpty()) { terrain.resized(kWorldSize*scale(size)).draw(toScreen({0, 0}, size), ColorF{1, .55}); }
			const Vec2 upper = toWorld({0, 56}, size), lower = toWorld({static_cast<double>(size.x), static_cast<double>(size.y)}, size);
			const RectF visibleWorld{upper, lower-upper};
			for (const int pass : {0, 1})
			{
				for (const auto& road : streets)
				{
					if (!road.bounds.intersects(visibleWorld) || (zoom<12 && road.category==0)) { continue; }
					const double width = Clamp(road.width*scale(size), road.category==0 ? 1.2 : 2.4, 24.0);
					const ColorF color = pass==0 ? ColorF{.68,.68,.64} : (road.category==2 ? ColorF{.35,.4,.46} : (road.category==1 ? ColorF{1,.83,.39} : ColorF{1}));
					for (size_t index=1; index<road.points.size(); ++index)
					{
						Line{toScreen(road.points[index-1], size), toScreen(road.points[index], size)}.draw(width+(pass==0 ? 2 : 0), color);
					}
				}
			}
			Array<RectF> occupied;
			for (const auto& label : labels)
			{
				const Vec2 point = toScreen(label.position, size);
				if (!body(size).stretched(-24).contains(point)) { continue; }
				const RectF bounds{point+Vec2{6,-10}, font(label.text).region().size+Vec2{12,8}};
				bool overlaps=false;
				for (const auto& used : occupied) { if (used.intersects(bounds)) { overlaps=true; break; } }
				if (overlaps) { continue; }
				occupied << bounds;
				if (label.station) { RectF{point-Vec2{4,4},8,8}.draw(ColorF{.18,.36,.55}); }
				bounds.draw(ColorF{1,.85});
				font(label.text).draw(bounds.pos+Vec2{6,2},ColorF{.17,.24,.29});
			}
			Circle{toScreen(camera, size), 7}.draw(ColorF{.1,.45,.94}).drawFrame(2, Palette::White);
		}
		RectF{0, 0, size.x, 56}.draw(ColorF{1});
		font(U"街の地図").draw(18, 14, ColorF{.13,.22,.29});
		font(U"ドラッグで移動  /  ホイールで拡大  /  右クリックで移動先を選択").draw(130, 17, ColorF{.35});
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
		if (contextWorld)
		{
			menuBounds().movedBy(2,3).draw(ColorF{0,.16});
			menuBounds().draw(Palette::White).drawFrame(1, ColorF{.7});
			font(U"ここへジャンプ").draw(contextScreen+Vec2{18,13}, ColorF{.13,.32,.51});
		}
	}
private:
	void clampCenter() { center.x=Clamp(center.x,0.0,kWorldSize); center.y=Clamp(center.y,0.0,kWorldSize); }
};
