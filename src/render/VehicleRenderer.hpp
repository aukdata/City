#pragma once
#include "../traffic/Vehicle.hpp"

/// @brief 車両の描画クラス
class VehicleRenderer
{
public:
	/// @brief 全車両を描画する
	void render(const Array<Vehicle>& vehicles, Vec3 cameraPos);
	/// @brief Moving vehicles share the visible model pose in the dynamic sun-depth pass.
	void renderShadowCasters(const Array<Vehicle>& vehicles, Vec3 focus, double radius);

	/// @brief 選択アウトライン用: 単一車両を単色で描画する
	void drawVehicleSilhouette(const Vehicle& v, Vec3 cameraPos, const ColorF& color);

private:
	/// @brief 単一車両を描画する（isClose で Model / Box を切り替え）
	void drawVehicle(const Vehicle& v, bool isClose);

	/// @brief 車種に対応するOBJを遅延ロードして返す
	Model& ensureVehicleModel(const Vehicle& v);

	/// @brief 車両のワールド変換行列（Model 用: car.obj の X=前方, Y=上, Z=横）を計算する
	static Mat4x4 carModelWorldMatrix(const Vehicle& v);

	/// @brief 近距離の乗用車・軽・バス・緊急車両をOBJで描くか
	static bool usesCarModel(const Vehicle& v, bool isClose);

	/// @brief 車種別3Dモデルの共有キャッシュ
	HashTable<String, Model> m_vehicleModels;
};
