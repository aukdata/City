#include "PedestrianRenderer.hpp"
#include "RailStructure.hpp"
#include "../gen/GenerationSettings.hpp"

namespace
{
/// @brief 閉じた箱を人体の部位として追加する。メッシュの生成は初回だけ。
void box(MeshData& mesh, Vec3 center, Vec3 size, double swing = 0)
{
	MeshData part;
	RailStructure::prism(part, {0, 0, -size.z * .5}, {0, 0, size.z * .5}, {1, 0, 0}, {1, 0, 0}, 0, size.x * .5,
		size.y * .5, -size.y * .5);
	for (auto& vertex : part.vertices)
	{
		const double y = vertex.pos.y, z = vertex.pos.z;
		vertex.pos =
			Float3{Vec3{vertex.pos.x, y * Cos(swing) - z * Sin(swing), y * Sin(swing) + z * Cos(swing)} + center};
		const double ny = vertex.normal.y, nz = vertex.normal.z;
		vertex.normal = Float3{vertex.normal.x, static_cast<float>(ny * Cos(swing) - nz * Sin(swing)),
			static_cast<float>(ny * Sin(swing) + nz * Cos(swing))};
	}
	BridgeStructure::append(mesh, part);
}
Float4 tint(ColorF color)
{
	color = color.removeSRGBCurve();
	return {static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), 1};
}
} // namespace
void PedestrianRenderer::prepare()
{
	if (m_ready)
	{
		return;
	}
	m_ready = true;
	for (size_t pose = 0; pose < kPoses; ++pose)
	{
		const double swing = Sin(pose * Math::TwoPi / kPoses) * .42;
		auto& shirt = m_near[pose][0].source.geometry;
		auto& skin = m_near[pose][1].source.geometry;
		auto& trousers = m_near[pose][2].source.geometry;
		box(shirt, {0, 1.12, 0}, {.40, .57, .24});
		box(skin, {0, 1.59, 0}, {.23, .27, .22});
		box(trousers, {0, 1.74, -.015}, {.235, .08, .235});
		for (const int side : {-1, 1})
		{
			box(shirt, {side * .26, 1.11, side * .10 * Sin(swing)}, {.12, .52, .14}, side * swing);
			box(skin, {side * .26, .81, side * .24 * Sin(swing)}, {.105, .12, .12});
			box(trousers, {side * .12, .44, -side * .15 * Sin(swing)}, {.15, .78, .17}, -side * swing);
			box(trousers, {side * .12, .075, .07 - side * .30 * Sin(swing)}, {.17, .13, .30});
		}
		for (auto& part : m_near[pose])
		{
			part.source.material.diffuse = ColorF{1};
		}
	}
	box(m_far.source.geometry, {0, .86, 0}, {.38, 1.48, .25});
	box(m_far.source.geometry, {0, 1.64, 0}, {.22, .24, .22});
	m_far.source.material.diffuse = ColorF{1};
}
void PedestrianRenderer::render(
	const Array<Pedestrian>& people, double now, const BasicCamera3D& camera, const std::function<bool(Vec3)>& visible)
{
	prepare();
	m_stats = {};
	m_far.batch.clear();
	for (auto& pose : m_near)
	{
		for (auto& part : pose)
		{
			part.batch.clear();
		}
	}
	const double maximum = GenerationSettings::get().pedestrians_drawDistance,
				 detail = GenerationSettings::get().pedestrians_detailedDrawDistance;
	const ViewFrustum frustum{camera, maximum};
	const Vec3 eye = camera.getEyePosition();
	const std::array<ColorF, 10> clothes{ColorF{.25, .42, .59}, ColorF{.76, .35, .26}, ColorF{.33, .52, .34},
		ColorF{.80, .70, .43}, ColorF{.39, .30, .48}, ColorF{.70, .73, .72}, ColorF{.17, .22, .27},
		ColorF{.50, .61, .64}, ColorF{.71, .51, .59}, ColorF{.49, .40, .28}};
	for (const auto& person : people)
	{
		const bool walking = person.state == PedestrianState::Walking;
		if (!walking && person.state != PedestrianState::WaitingCar && person.state != PedestrianState::WaitingTrain &&
			person.state != PedestrianState::Planning)
		{
			continue;
		}
		Vec3 position = person.position;
		if (visible && !visible(position))
		{
			continue;
		}
		const double distance = position.distanceFromSq(eye);
		if (distance > maximum * maximum || !frustum.intersects(Sphere{position + Vec3{0, .9, 0}, 1.1}))
		{
			continue;
		}
		// 個体ごとの小さな横ずれだけを描画に加え、共通の移動経路は変えない。
		const Vec3 right{Cos(person.heading), 0, -Sin(person.heading)};
		position += right * ((person.id % 7 - 3) * .07);
		if (!walking)
		{
			position += Vec3{(person.id % 7 - 3) * .32, 0, ((person.id / 7) % 7 - 3) * .32};
		}
		const float scale = static_cast<float>(.90 + (person.id % 17) * .011);
		const Mat4x4 transform = (Mat4x4::Scale(scale) * Mat4x4::RotateY(person.heading))
									 .translated(static_cast<float>(position.x), static_cast<float>(position.y),
										 static_cast<float>(position.z));
		const Float4 paint = tint(clothes[person.id % clothes.size()]);
		++m_stats.submitted;
		if (distance > detail * detail)
		{
			m_far.batch.append(m_far.source, transform, paint);
			continue;
		}
		++m_stats.detailed;
		const size_t pose = walking ? static_cast<size_t>(now * person.speed * 6 + person.id) % kPoses : 0;
		m_near[pose][0].batch.append(m_near[pose][0].source, transform, paint);
		m_near[pose][1].batch.append(m_near[pose][1].source, transform,
			tint(ColorF{.80 + (person.id % 5) * .018, .61 + (person.id % 3) * .035, .46 + (person.id % 4) * .025}));
		m_near[pose][2].batch.append(m_near[pose][2].source, transform, tint(ColorF{.16, .19, .24}));
	}
	for (auto& pose : m_near)
	{
		for (auto& part : pose)
		{
			m_stats.triangles += part.batch.triangles();
			m_stats.drawCalls += part.batch.draw();
		}
	}
	m_stats.triangles += m_far.batch.triangles();
	m_stats.drawCalls += m_far.batch.draw();
}
