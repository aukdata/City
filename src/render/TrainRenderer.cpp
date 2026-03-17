#include "TrainRenderer.hpp"

void TrainRenderer::renderTracks(const TrainNetwork& network)
{
	constexpr float kTrackWidth  = 1.435f;  // 軌間 [m]（標準軌）
	constexpr int   kSegments    = 50;

	for (const auto& edge : network.edges())
	{
		if (!edge.isValid()) continue;
		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		// レールのポリゴン帯
		Array<Vec3> leftRail, rightRail;
		leftRail.reserve(kSegments + 1);
		rightRail.reserve(kSegments + 1);

		for (int i = 0; i <= kSegments; ++i)
		{
			const float t = static_cast<float>(i) / kSegments;
			const Vec3 pos     = bez->evaluate(t);
			const Vec3 tangent = bez->tangent(t).normalized();
			const Vec3 right   = Vec3{ tangent.z, 0, -tangent.x };  // XZ 面の右方向

			leftRail  << pos - right * (kTrackWidth * 0.5);
			rightRail << pos + right * (kTrackWidth * 0.5);
		}

		// 左右レールを細いシリンダーで描画
		const ColorF railColor{ 0.55, 0.55, 0.60 };
		for (int i = 0; i < kSegments; ++i)
		{
			Cylinder{ leftRail[i]  + Vec3{0,0.1,0},
			          leftRail[i+1]+ Vec3{0,0.1,0}, 0.07 }.draw(railColor);
			Cylinder{ rightRail[i] + Vec3{0,0.1,0},
			          rightRail[i+1]+Vec3{0,0.1,0}, 0.07 }.draw(railColor);
		}

		// 枕木（5m ごと）
		const ColorF sleeperColor{ 0.35, 0.25, 0.18 };
		const int numSleepers = Max(1, static_cast<int>(edge.length / 5.0f));
		for (int si = 0; si <= numSleepers; ++si)
		{
			const float t = static_cast<float>(si) / numSleepers;
			const Vec3 pos     = bez->evaluate(t);
			const Vec3 tangent = bez->tangent(t).normalized();
			const Vec3 right   = Vec3{ tangent.z, 0, -tangent.x };
			const Vec3 left2   = pos - right * (kTrackWidth * 0.5 + 0.2);
			const Vec3 right2  = pos + right * (kTrackWidth * 0.5 + 0.2);
			Cylinder{ left2, right2, 0.13 }.draw(sleeperColor);
		}
	}

	// 駅名ラベル（カメラが近いとき）
	for (const auto& node : network.nodes())
	{
		if (node.type == TrackNodeType::Station && !node.name.isEmpty())
		{
			// 駅マーカー
			Cylinder{ node.position, node.position + Vec3{0,8,0}, 3.0 }
				.draw(ColorF{ 0.9, 0.85, 0.2 });
		}
	}
}

void TrainRenderer::renderTrains(const Array<Train>& trains)
{
	for (const auto& train : trains)
	{
		if (train.currentEdge < 0) continue;

		// 車体のボックス（シンプルな直方体）
		const ColorF bodyColor = (train.type == TrainType::Shinkansen)
			? ColorF{ 0.9, 0.95, 1.0 }
			: (train.type == TrainType::Express || train.type == TrainType::LimitedExpress)
			? ColorF{ 0.2, 0.4, 0.8 }
			: ColorF{ 0.3, 0.6, 0.35 };

		// 列車の向きに合わせて回転させる
		const Quaternion rot = Quaternion::RotationAxis(Float3{ 0, 1, 0 },
		                                                -train.heading);

		// 車体サイズ: 長さ 20m × 幅 3m × 高さ 3.5m
		Box{ train.position + Vec3{0, 1.75, 0}, 3.0, 3.5, 20.0 }
			.draw(rot, bodyColor);

		// 前面ライン
		Box{ train.position + Vec3{0, 2.5, 0}, 3.1, 0.3, 0.4 }
			.draw(rot, ColorF{ 0.1, 0.1, 0.1 });
	}
}
