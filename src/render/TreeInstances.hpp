#pragma once
#include <Siv3D.hpp>

/// @brief 全LODで共有する、木一本の位置・非一様スケール・Y回転。
struct TreeTransform
{
	Float4 positionWidth;
	Float4 heightRotation; ///< 高さ、cos(angle)、sin(angle)、予備。
};

static_assert(sizeof(TreeTransform) == 32);

/// @brief 頂点を複製せず、原型番号と配置だけを保持する植栽。
struct TreeInstance
{
	TreeTransform transform;
	uint8 model = 0;
	uint8 palette = 0;
	uint8 tile = 0; ///< 配置時に確定する256mタイル。描画中に座標を再分解しない。
};

/// @brief 固定GPU形状を共有し、描画する個体の変換だけをまとめて転送する。
class TreeInstanceRenderer
{
public:
	/// @brief 少数の原型と全LODをロード画面でGPUへ用意する。
	static void preload();
	void clear();
	/// @brief 配置済みの木を256mタイルの距離で振り分ける。配置生成は行わない。
	void append(const Array<TreeInstance>& trees, Point chunk, Vec2 heights, Vec3 eye);
	/// @brief 影描画では呼び出し元の深度ピクセルシェーダーを保持する。
	void draw(const PixelShader& foliage, bool shadowPass = false);
	[[nodiscard]] size_t nearInstances() const { return m_nearInstances; }
	[[nodiscard]] size_t farInstances() const { return m_farInstances; }
	static constexpr uint8 kShrubModel = 16;
	static constexpr size_t kModelCount = 17;
	static constexpr uint32 kNearCapacity = 64, kFarCapacity = 1024;

private:
	static constexpr size_t kBucketCount = kModelCount * 2 * 2;
	struct NearBuffer { std::array<TreeTransform, kNearCapacity> values; };
	struct FarBuffer { std::array<TreeTransform, kFarCapacity> values; };
	std::array<Array<TreeTransform>, kBucketCount> m_buckets;
	ConstantBuffer<NearBuffer> m_nearInstancesBuffer;
	ConstantBuffer<FarBuffer> m_farInstancesBuffer;
	size_t m_nearInstances = 0, m_farInstances = 0;
};
