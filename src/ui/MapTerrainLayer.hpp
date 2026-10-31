#pragma once
#include <Siv3D.hpp>

/// @brief Terrain shading cache. A bounded update preserves the complete previous tile until publication.
class MapTerrainLayer
{
public:
	RectF bounds;
	DynamicTexture texture;
	using WaterDepth = std::function<double(Vec2,float)>;
	/// @brief Synchronous water-aware update for rendering verification.
	bool update(RectF visible, Size pixels, const std::function<float(Vec2)>& sample, const WaterDepth& waterDepth)
	{ return update(visible,pixels,sample,Math::Inf,waterDepth); }
	double metersPerPixel = 0;
	void invalidate() { m_valid = false; m_build.reset(); }
	/// @brief 水面の変化などで描き直す。作りかけの陰影は捨てずに仕上げて公開し、その後に描き直す。
	void refresh()
	{
		m_valid = false;
		if (m_build) { m_build->stale = true; }
	}
	/// @brief 最後に求められた表示範囲の陰影図を公開済みで、作りかけの陰影がないか。
	bool valid() const { return m_valid && !m_build; }
	/// @brief Sample and shade a padded tile, synchronously by default or a few rows per frame.
	bool update(RectF visible, Size pixels, const std::function<float(Vec2)>& sample, double budgetMs = Math::Inf, const WaterDepth& waterDepth = {})
	{
		const double requested = visible.w * 1.5 / pixels.x;
		if (m_valid && covers(bounds, texture.size(), metersPerPixel, visible, pixels, requested))
		{
			m_build.reset(); return false;
		}
		const Stopwatch timer{StartImmediately::Yes};
		// まだ何も表示していないときは 1 フレームの予算を広げ、最初の陰影図を早く出す。
		if (!texture) { budgetMs = Max(budgetMs, 6.0); }
		if (!m_build || !covers(m_build->bounds, m_build->pixels, m_build->bounds.w / pixels.x, visible, pixels, requested))
		{
			const Vec2 cell = visible.size / 8;
			const Vec2 center{Math::Floor(visible.center().x / cell.x) * cell.x, Math::Floor(visible.center().y / cell.y) * cell.y};
			m_build.emplace(Build{RectF{center - visible.size * .75, visible.size * 1.5}, pixels,
				Grid<float>{static_cast<size_t>(pixels.x + 2), static_cast<size_t>(pixels.y + 2)}, Image{pixels, Color{0,0,0,0}}});
		}
		auto& tile = *m_build;
		const Vec2 step{tile.bounds.w / pixels.x, tile.bounds.h / pixels.y};
		while (tile.heightRow < pixels.y + 2 && timer.msF() < budgetMs)
		{
			const int y = tile.heightRow++;
			for (int x = 0; x < pixels.x + 2; ++x)
			{
				const Vec2 world = tile.bounds.pos + Vec2{(x - .5) * step.x, (y - .5) * step.y};
				tile.heights[y][x] = sample({Clamp(world.x, 0.0, 65535.99), Clamp(world.y, 0.0, 65535.99)});
			}
		}
		while (tile.heightRow == pixels.y + 2 && tile.imageRow < pixels.y && timer.msF() < budgetMs)
		{
			const int y = tile.imageRow++;
			for (int x = 0; x < pixels.x; ++x)
			{
				const float height = tile.heights[y + 1][x + 1];
				const double dx = (tile.heights[y + 1][x + 2] - tile.heights[y + 1][x]) / (2 * step.x);
				const double dz = (tile.heights[y + 2][x + 1] - tile.heights[y][x + 1]) / (2 * step.y);
				const Vec2 world=tile.bounds.pos+Vec2{(x+.5)*step.x,(y+.5)*step.y};
				const double wet=waterDepth ? waterDepth({Clamp(world.x,0.0,65535.99),Clamp(world.y,0.0,65535.99)},height) : Max(0.0,-static_cast<double>(height));
				const double light = wet > .003 ? 1 : Clamp(.9 + (-dx * .55 - dz * .45) / Math::Sqrt(1 + dx * dx + dz * dz), .57, 1.18);
				const ColorF color = wet > .003 ? ColorF{.37,.62,.75}.lerp(ColorF{.18,.4,.61}, Clamp(wet / 70.0, 0.0, 1.0))
					: height < 180 ? ColorF{.69,.77,.56}.lerp(ColorF{.51,.64,.43}, Clamp(height / 180.0, 0.0, 1.0))
					: ColorF{.51,.64,.43}.lerp(ColorF{.82,.81,.74}, Clamp((height - 180) / 1100.0, 0.0, 1.0));
				tile.image[y][x] = ColorF{color.r * light, color.g * light, color.b * light};
			}
		}
		if (tile.imageRow != pixels.y) { return false; }
		if (texture.size() != pixels) { texture = DynamicTexture{tile.image}; }
		else
		{
			const ScopedCustomShader2D restoreShaders{VertexShader{}, PixelShader{}};
			Graphics2D::Flush();
			if (!texture.fill(tile.image)) { texture = DynamicTexture{tile.image}; }
		}
		bounds = tile.bounds; metersPerPixel = bounds.w / pixels.x;
		m_valid = !tile.stale; m_build.reset(); return true;
	}
private:
	struct Build
	{
		RectF bounds;
		Size pixels;
		Grid<float> heights;
		Image image;
		int heightRow = 0, imageRow = 0;
		bool stale = false;
	};
	bool m_valid = false;
	Optional<Build> m_build;
	static bool covers(RectF bounds, Size size, double spacing, RectF visible, Size pixels, double requested)
	{
		return size == pixels && bounds.contains(visible.tl()) && bounds.contains(visible.br())
			&& spacing <= requested * 1.1 && spacing >= requested * .6;
	}
};
