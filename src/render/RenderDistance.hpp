#pragma once
#include <Siv3D.hpp>
#include <charconv>

/// @brief 建物・植栽だけへ追加する描画距離。0 は従来の視錐台・LOD制限をそのまま使う。
namespace RenderDistance
{
	inline constexpr double kDefault = 0.0;
	inline constexpr double kMinimum = 100.0;
	inline constexpr double kMaximum = 20000.0;
	inline constexpr double kBuildingMargin = 2.0; ///< モデル外周の玄関・庇等を残す余裕 [m]。

	[[nodiscard]] inline bool valid(double meters)
	{
		return IsFinite(meters) && (meters == kDefault || InRange(meters, kMinimum, kMaximum));
	}

	/// @brief UI と CLI の共通パーサー。無効値で既存設定を上書きしない。
	[[nodiscard]] inline Optional<double> parse(StringView input)
	{
		const std::string text = Unicode::ToUTF8(input);
		const char* first = text.data();
		const char* const last = first + text.size();
		if (first != last && *first == '+')
		{
			++first;
			if (first != last && *first == '-') { return none; }
		}
		double value = 0.0;
		const auto parsed = std::from_chars(first, last, value);
		if (parsed.ec != std::errc{} || parsed.ptr != last || !valid(value)) { return none; }
		return value;
	}

	/// @brief 原点ではなくジオメトリ外接球の手前端から測る。境界に接する物体を残す。
	[[nodiscard]] inline bool contains(Vec3 eye, const Sphere& bounds, double meters)
	{
		return meters == kDefault || eye.distanceFromSq(bounds.center) <= Square(meters + bounds.r);
	}

	/// @brief AABB への最短3D距離。大きな結合バッチを部分的に切らない。
	[[nodiscard]] inline bool contains(Vec3 eye, const Box& bounds, double meters)
	{
		if (meters == kDefault) { return true; }
		const Vec3 delta = eye - bounds.center;
		const Vec3 outside{Max(0.0, Abs(delta.x) - bounds.size.x * .5),
			Max(0.0, Abs(delta.y) - bounds.size.y * .5), Max(0.0, Abs(delta.z) - bounds.size.z * .5)};
		return outside.lengthSq() <= Square(meters);
	}

	/// @brief モデルと選択箱が共有する保守的な境界。
	[[nodiscard]] inline Sphere buildingSphere(const OrientedBox& bounds)
	{
		return Sphere{bounds.center, bounds.size.length() * .5 + kBuildingMargin};
	}

	/// @brief Siv3D の scale → RotateY → translate と同じモデル境界変換。
	[[nodiscard]] inline OrientedBox transformBox(const Box& bounds, double scale, double yaw, Vec3 position)
	{
		const Vec3 center = bounds.center * scale;
		const double cosine = Cos(yaw), sine = Sin(yaw);
		return OrientedBox{position + Vec3{center.x * cosine + center.z * sine, center.y,
			-center.x * sine + center.z * cosine}, bounds.size * scale, Quaternion::RotateY(yaw)};
	}

	/// @brief 複数の実ジオメトリを囲む AABB。チャンク原点による早すぎる消失を避ける。
	[[nodiscard]] inline Box merge(const Box& a, const Box& b)
	{
		const Vec3 aMin = a.center - a.size * .5, aMax = a.center + a.size * .5;
		const Vec3 bMin = b.center - b.size * .5, bMax = b.center + b.size * .5;
		const Vec3 lower{Min(aMin.x,bMin.x),Min(aMin.y,bMin.y),Min(aMin.z,bMin.z)};
		const Vec3 upper{Max(aMax.x,bMax.x),Max(aMax.y,bMax.y),Max(aMax.z,bMax.z)};
		return Box{(lower + upper) * .5, upper - lower};
	}

	/// @brief 地面・田畑の面は残し、建物・壁・設備・樹冠を距離制限の対象にする。
	[[nodiscard]] inline bool limitedMaterial(int key)
	{
		switch (key % 1000)
		{
		case 100: case 101: case 106: case 109: case 110: case 111: case 113:
		case 114: case 119: case 120: case 121: case 122: case 124: case 127:
		case 130: case 131:
			return false;
		default:
			return true;
		}
	}
}
