#pragma once
#include "BezierUtil.hpp"
#include "../world/World.hpp"

/// @brief 覆工と屋外設備が共有する、中心線からの土被り判定。
namespace RoadEnvironment
{
	inline constexpr double kRoadTunnelCrown = 5.8;
	inline constexpr double kRailTunnelCrown = 6.5;
	inline constexpr double kMinimumCover = 1.0;
	inline constexpr float kSectionStep = 3.0f;

	[[nodiscard]] inline bool coveredAt(const World& world, Vec3 position, bool tunnelCandidate,
		double crown = kRoadTunnelCrown)
	{
		return tunnelCandidate && world.sampleHeight(static_cast<float>(position.x),static_cast<float>(position.z))
			- position.y >= crown + kMinimumCover;
	}

	/// @brief 両端が屋外でも、短いトンネルを架空線が横切らないよう区間全体を調べる。
	[[nodiscard]] inline bool outdoorSpan(const World& world,const CubicBezier& curve,
		float start,float end,bool tunnelCandidate)
	{
		if (!tunnelCandidate) { return true; }
		const int count = Max(1,static_cast<int>(Ceil(Abs(end-start)/kSectionStep)));
		for (int i=0;i<=count;++i)
		{
			if (coveredAt(world,curve.positionAt(Math::Lerp(start,end,static_cast<float>(i)/count)),true)) { return false; }
		}
		return true;
	}
}
