#pragma once
#include "PlainLabel.hpp"
#include "Camera.hpp"
#include "NavigationHeader.hpp"

/// @brief 街の位置に追従する俯瞰用ラベル。画面内の重なりを除いてから描く。
namespace TownBillboards
{
	struct Place { Vec3 position; String name; int importance = 0; String reading; };
	struct Label { Vec2 anchor; RectF bounds; String name; double fontSize; String reading; };

	inline Array<Label> layout(const Array<Place>& places, const GameCamera& camera, Size size, const Font& font, const Array<RectF>& excluded = {})
	{
		if (camera.mode() != CameraMode::Overview && camera.mode() != CameraMode::Capture) { return {}; }
		Array<size_t> order;
		for (size_t i = 0; i < places.size(); ++i) { order << i; }
		order.sort_by([&](size_t a, size_t b)
		{
			if (places[a].importance != places[b].importance) { return places[a].importance > places[b].importance; }
			return places[a].position.distanceFromSq(camera.focusPoint()) < places[b].position.distanceFromSq(camera.focusPoint());
		});
		const Vec3 eye = camera.eyePosition(), forward = camera.focusPoint() - eye;
		const RectF visible{8, 94, size.x - 16.0, Max(0.0, size.y - 204.0)};
		Array<Label> labels;
		for (const size_t i : order)
		{
			const auto& place = places[i];
			if (place.name.isEmpty() || (place.position - eye).dot(forward) <= 0) { continue; }
			const double reach = place.importance == 2 ? 120000 : place.importance == 1 ? 45000 : 15000;
			if (place.position.distanceFromSq(eye) > reach * reach) { continue; }
			const Float3 projected = camera.camera3D().worldToScreenPoint(Float3{place.position});
			const Vec2 anchor{projected.x, projected.y};
			const double fontSize = place.importance == 2 ? 21 : place.importance == 1 ? 18 : 15;
			const double width = Min(260.0, Max(font(place.name).region(fontSize).w,font(place.reading).region(12).w)+22);
			const double height = place.reading.isEmpty() ? 29 : 45;
			const RectF bounds{anchor.x-width*.5,anchor.y-height-9,width,height};
			if (!visible.contains(bounds.tl()) || !visible.contains(bounds.br())) { continue; }
			bool overlap = false;
			for (const auto& blocked : excluded) { overlap |= bounds.stretched(4).intersects(blocked); }
			for (const auto& label : labels) { overlap |= bounds.stretched(8, 6).intersects(label.bounds); }
			if (overlap) { continue; }
			labels << Label{anchor, bounds, place.name, fontSize, place.reading};
			if (labels.size() == 24) { break; }
		}
		return labels;
	}

	inline void draw(const Array<Label>& labels, const Font& font)
	{
		for (const auto& label : labels)
		{
			Line{Vec2{label.anchor.x, label.bounds.bottomY()}, label.anchor}.draw(1.5, ColorF{.85, .94, .95, .8});
			Circle{label.anchor, 2.5}.draw(ColorF{.85, .94, .95});
			PlainLabel::fitted(font,label.name,label.fontSize,{label.bounds.pos+Vec2{8,2},label.bounds.w-16,25},Palette::White,ColorF{.05},true);
			if (!label.reading.isEmpty()) { PlainLabel::fitted(font,label.reading,12,{label.bounds.pos+Vec2{8,27},label.bounds.w-16,14},ColorF{.82,.90,.94},ColorF{.05},true); }
		}
	}
}
