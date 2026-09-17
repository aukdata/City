#pragma once
#include "../road/RoadNetwork.hpp"
#include <limits>

/// @brief 初期道路生成中に追加したノードの平面位置を検索する索引。
/// @details 距離内の候補列挙だけを共通化し、吸着距離・街道への接続費などの選択方針は呼出側が決める。
/// ノードの追加時に insert を呼ぶ。既存ノードを移動した場合は索引を作り直す。
class RoadNodeIndex
{
public:
	/// @brief 接続先の候補集合が切り替わる生成段階で、登録ノードを空にする。
	void clear() { m_buckets.clear(); }

	/// @brief ノード ID を登録する。削除済み ID は検索時に除外する。
	void insert(Vec3 position, int nodeId)
	{
		m_buckets[key(bucket(static_cast<float>(position.x)), bucket(static_cast<float>(position.z)))] << nodeId;
	}

	/// @brief 半径内で評価値が最小のノードを返す。score が none を返した候補は採用しない。
	/// @details 半径ちょうどの点を除外し、同点では従来のバケット走査順で最初の候補を保持する。
	template <class Score>
	[[nodiscard]] Optional<int> findBest(Vec3 position, const RoadNetwork& network, float radius, Score score) const
	{
		if (!(radius > 0.0f)) { return none; }
		const float x = static_cast<float>(position.x);
		const float z = static_cast<float>(position.z);
		const int range = static_cast<int>(Ceil(radius / kBucketSize));
		const int bx = bucket(x), bz = bucket(z);
		const float radiusSq = radius * radius;
		double bestCost = std::numeric_limits<double>::infinity();
		Optional<int> best;

		for (int dz = -range; dz <= range; ++dz)
		{
			for (int dx = -range; dx <= range; ++dx)
			{
				const auto it = m_buckets.find(key(bx + dx, bz + dz));
				if (it == m_buckets.end()) { continue; }
				for (const int id : it->second)
				{
					const RoadNode* node = network.getNode(id);
					if (!node) { continue; }
					const float offsetX = static_cast<float>(node->position.x) - x;
					const float offsetZ = static_cast<float>(node->position.z) - z;
					const float distanceSq = offsetX * offsetX + offsetZ * offsetZ;
					if (distanceSq >= radiusSq) { continue; }
					const Optional<double> cost = score(*node, distanceSq);
					if (cost && *cost < bestCost)
					{
						bestCost = *cost;
						best = id;
					}
				}
			}
		}
		return best;
	}

	/// @brief 地区街路の吸着先。接続前の孤立ノードも距離だけで選ぶ。
	[[nodiscard]] Optional<int> findNearest(Vec3 position, const RoadNetwork& network, float radius) const
	{
		return findBest(position, network, radius, [](const RoadNode&, float distanceSq) -> Optional<double>
		{
			return distanceSq;
		});
	}

private:
	static constexpr float kBucketSize = 200.0f;
	HashTable<uint64, Array<int>> m_buckets;

	[[nodiscard]] static int bucket(float coordinate)
	{
		return static_cast<int>(Math::Floor(coordinate / kBucketSize));
	}

	[[nodiscard]] static uint64 key(int x, int z)
	{
		// 負の座標も符号付き左シフトにせず、各軸の32ビットをそのまま連結する。
		return (static_cast<uint64>(static_cast<uint32>(x)) << 32) | static_cast<uint32>(z);
	}
};
