#pragma once
#include <Siv3D.hpp>
#include "MeshGeometry.hpp"
#include "../gen/PowerLines.hpp"

/// @brief 送電鉄塔（2 回線垂直配列の四角鉄塔）と架空線のメッシュ。
/// @details 154〜275kV 級の山形鋼トラス鉄塔を簡略化する。脚は腰部まで絞り、上部に 3 段の腕金と頂部の架空地線腕を持つ。
///   架空線は懸垂がいしの下端どうしを放物線で結び、道路の電線と同じ画面幅のケーブルシェーダで描く。
namespace PowerLineGeometry
{
	/// @brief 任意方向の角材（四角柱）。側面だけを持ち、端面は隣の部材に隠れる。
	inline void appendBeam(MeshData& mesh, Vec3 a, Vec3 b, double thickness)
	{
		const Vec3 axis = b - a;
		const double length = axis.length();
		if (length < 1e-4) { return; }
		const Vec3 d = axis / length;
		const Vec3 helper = Abs(d.y) < .9 ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
		const Vec3 u = d.cross(helper).normalized() * (thickness * .5), v = d.cross(u).normalized() * (thickness * .5);
		const std::array<Vec3, 4> corners{ u + v, -u + v, -u - v, u - v };
		for (size_t i = 0; i < 4; ++i)
		{
			const Vec3 c0 = corners[i], c1 = corners[(i + 1) % 4];
			const Float3 normal{ (c0 + c1).normalized() };
			const uint32 base = static_cast<uint32>(mesh.vertices.size());
			for (const Vec3 p : { a + c0, a + c1, b + c1, b + c0 }) { mesh.vertices << Vertex3D{ Float3{ p }, normal, Float2{ 0, 0 } }; }
			mesh.indices << TriangleIndex32{ base, base + 2, base + 1 } << TriangleIndex32{ base, base + 3, base + 2 };
		}
	}

	/// @brief 腕金の取付高さ（全高に対する比）。上から上相・中相・下相。
	inline constexpr std::array<double, 3> kArmLevels{ .91, .80, .69 };
	inline constexpr double kWaist = .62;     ///< 脚の絞りが終わる高さ（全高比）
	inline constexpr double kInsulator = 2.6; ///< 懸垂がいし連の長さ [m]

	/// @brief 腕の先端までの水平距離（中相を長くして相間の離隔を取る）。
	inline double armReach(double height, size_t level) { return height * (level == 1 ? .15 : .125); }

	/// @brief 塔体の半幅（地表で広く、腰部から上は一定）。
	inline double halfWidth(double height, double y)
	{
		const double waist = height * kWaist, top = height * .028;
		if (y >= waist) { return top; }
		return Math::Lerp(height * .085, top, y / waist);
	}

	/// @brief 架空線の取付点。0〜5 が電力線（左右×3 段）、6・7 が頂部の架空地線。
	inline Array<Vec3> attachPoints(const PowerLines::Tower& tower, double height)
	{
		const Vec3 right{ tower.along.y, 0, -tower.along.x };
		Array<Vec3> points;
		for (const double side : { -1.0, 1.0 })
		{
			for (size_t level = 0; level < kArmLevels.size(); ++level)
			{
				// 耐張鉄塔のがいしは水平に張るため、取付点は腕の高さのまま。
				const double drop = tower.angle ? .3 : kInsulator;
				points << tower.base + right * (side * armReach(height, level)) + Vec3{ 0, height * kArmLevels[level] - drop, 0 };
			}
		}
		for (const double side : { -1.0, 1.0 }) { points << tower.base + right * (side * height * .07) + Vec3{ 0, height + .4, 0 }; }
		return points;
	}

	/// @brief 鉄塔 1 基を steel（鋼材）と insulator（がいし）へ追加する。
	inline void appendTower(MeshData& steel, MeshData& insulator, MeshData& concrete, const PowerLines::Tower& tower, double height)
	{
		const Vec3 forward{ tower.along.x, 0, tower.along.y }, right{ tower.along.y, 0, -tower.along.x };
		const auto corner = [&](double y, double sx, double sz)
		{
			const double w = halfWidth(height, y);
			return tower.base + right * (sx * w) + forward * (sz * w) + Vec3{ 0, y, 0 };
		};
		constexpr std::array<std::pair<double, double>, 4> kCorners{ std::pair{ -1.0, -1.0 }, { 1.0, -1.0 }, { 1.0, 1.0 }, { -1.0, 1.0 } };
		// 主柱材の節点。脚部は下ほど間隔を広く、上部は腕金の段に合わせる。
		Array<double> levels{ 0 };
		for (const double t : { .2, .36, .5, .62, .74, .86, 1.0 }) { levels << height * kWaist * t; }
		for (const double r : { kArmLevels[2], kArmLevels[1], kArmLevels[0], 1.0 }) { levels << height * r; }
		const double leg = Max(.16, height * .0045), brace = leg * .55;
		for (size_t i = 0; i + 1 < levels.size(); ++i)
		{
			const double y0 = levels[i], y1 = levels[i + 1];
			for (size_t c = 0; c < 4; ++c)
			{
				const auto [ax, az] = kCorners[c];
				const auto [bx, bz] = kCorners[(c + 1) % 4];
				appendBeam(steel, corner(y0, ax, az), corner(y1, ax, az), leg);
				// 各面の X 形の斜材と水平材。
				appendBeam(steel, corner(y0, ax, az), corner(y1, bx, bz), brace);
				appendBeam(steel, corner(y0, bx, bz), corner(y1, ax, az), brace);
				appendBeam(steel, corner(y1, ax, az), corner(y1, bx, bz), brace);
			}
		}
		// 基礎（4 脚のコンクリート）。
		for (const auto& [sx, sz] : kCorners)
		{
			MeshGeometry::appendOrientedBox(concrete, corner(0, sx, sz) + Vec3{ 0, .2, 0 }, Float3{ 1.1f, 1.0f, 1.1f }, static_cast<float>(std::atan2(tower.along.y, tower.along.x)));
		}
		// 腕金: 上弦・下弦の 2 本を塔体から先端へ絞り、先端からがいし連を吊る。
		for (size_t level = 0; level < kArmLevels.size(); ++level)
		{
			const double y = height * kArmLevels[level], reach = armReach(height, level), w = halfWidth(height, y);
			for (const double side : { -1.0, 1.0 })
			{
				const Vec3 tip = tower.base + right * (side * reach) + Vec3{ 0, y, 0 };
				for (const double fz : { -1.0, 1.0 })
				{
					appendBeam(steel, corner(y, side, fz), tip, brace);
					appendBeam(steel, corner(y + w * 1.6, side, fz) , tip + Vec3{ 0, .15, 0 }, brace);
				}
				if (tower.angle)
				{
					for (const double fz : { -1.0, 1.0 }) { appendBeam(insulator, tip, tip + forward * (fz * kInsulator), .32); }
				}
				else { appendBeam(insulator, tip, tip - Vec3{ 0, kInsulator, 0 }, .32); }
			}
		}
		// 頂部の架空地線支持腕。
		for (const double side : { -1.0, 1.0 })
		{
			const Vec3 tip = tower.base + right * (side * height * .07) + Vec3{ 0, height + .4, 0 };
			for (const double fz : { -1.0, 1.0 }) { appendBeam(steel, corner(height, side, fz), tip, brace); }
		}
	}

	/// @brief 2 基の間の架空線。cable シェーダ用に方向を法線、横位置と半径を UV に入れる。
	inline void appendConductor(MeshData& cable, Vec3 from, Vec3 to, double sagRatio, double radius)
	{
		const double span = Vec2{ to.x - from.x, to.z - from.z }.length();
		const double sag = span * sagRatio;
		constexpr int kSegments = 18;
		Vec3 previous = from;
		for (int segment = 1; segment <= kSegments; ++segment)
		{
			const double t = static_cast<double>(segment) / kSegments;
			const Vec3 current = from.lerp(to, t) - Vec3{ 0, 4.0 * sag * t * (1.0 - t), 0 };
			const Vec3 delta = current - previous;
			const double length = delta.length();
			if (length > .001)
			{
				const uint32 base = static_cast<uint32>(cable.vertices.size());
				const Float3 direction{ delta / length };
				for (const Vec3 endpoint : { previous, current })
				{
					for (const float side : { -1.0f, 1.0f }) { cable.vertices << Vertex3D{ Float3{ endpoint }, direction, Float2{ side, static_cast<float>(radius) } }; }
				}
				cable.indices << TriangleIndex32{ base, base + 1, base + 2 } << TriangleIndex32{ base + 2, base + 1, base + 3 };
			}
			previous = current;
		}
	}
}
