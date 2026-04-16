#pragma once

/// @brief 接線ベクトルから XZ 平面の右方向（Y=0）を求める
/// @param tan  接線ベクトル（正規化済みでなくても可）
/// @return 正規化された右方向ベクトル。XZ 長さが極端に小さい場合は (1,0,0)
inline Vec3 tangentToRight(const Vec3& tan)
{
	const double lenXZ = Math::Sqrt(tan.x * tan.x + tan.z * tan.z);
	if (lenXZ > 1e-6)
		return Vec3{ tan.z / lenXZ, 0.0, -tan.x / lenXZ };
	return Vec3{ 1.0, 0.0, 0.0 };
}

/// @brief 3次ベジェ曲線ユーティリティ
/// @details B(t) = (1-t)³P0 + 3(1-t)²tP1 + 3(1-t)t²P2 + t³P3
struct CubicBezier
{
	Vec3 p0, p1, p2, p3;   ///< 始点・制御点A・制御点B・終点

	/// @brief 弧長テーブルのサンプル数
	static constexpr int SAMPLES = 50;

	Array<float> arcTable;   ///< サンプル点での累積弧長 (size = SAMPLES+1)
	float        totalLength = 0.0f;

	CubicBezier() = default;

	/// @brief コンストラクタ（弧長テーブルを自動構築する）
	CubicBezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3);

	/// @brief パラメータ t (0-1) での位置を返す
	Vec3 evaluate(float t) const;

	/// @brief パラメータ t (0-1) での正規化接線を返す
	Vec3 tangent(float t) const;

	/// @brief 弧長 s から t を逆引きする（二分探索）
	float tFromArcLength(float s) const;

	/// @brief 弧長 s での位置を返す
	Vec3 positionAt(float s) const { return evaluate(tFromArcLength(s)); }

	/// @brief 弧長 s での正規化接線を返す
	Vec3 tangentAt(float s) const { return tangent(tFromArcLength(s)); }

	/// @brief ド・カステリョ分割で2本の子ベジェに分割する
	/// @param t 分割パラメータ [0,1]
	/// @return {前半 [0,t], 後半 [t,1]} の CubicBezier ペア
	std::pair<CubicBezier, CubicBezier> split(float t) const;

private:
	void buildTable();
};
