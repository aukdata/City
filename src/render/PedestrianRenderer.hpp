#pragma once
#include "VehicleInstanceBatch.hpp"
#include "../pedestrian/PedestrianManager.hpp"

/// @brief 人体の共有メッシュと距離別LOD。近景の歩行姿勢も少数のバッチへまとめる。
class PedestrianRenderer
{
public:
	struct Stats
	{
		int submitted = 0, detailed = 0;
		uint32 drawCalls = 0;
		size_t triangles = 0;
	};
	void render(const Array<Pedestrian>& people, double now, const BasicCamera3D& camera,
		const std::function<bool(Vec3)>& visible = {});
	[[nodiscard]] const Stats& stats() const { return m_stats; }

private:
	static constexpr size_t kPoses = 8, kParts = 3;
	struct Part
	{
		ModelMeshSource source;
		VehicleInstanceBatch batch;
	};
	std::array<std::array<Part, kParts>, kPoses> m_near;
	Part m_far;
	bool m_ready = false;
	Stats m_stats;
	void prepare();
};
