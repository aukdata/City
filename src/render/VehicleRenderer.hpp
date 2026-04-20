#pragma once
#include "../traffic/Vehicle.hpp"

/// @brief 車両の描画クラス
class VehicleRenderer
{
public:
	/// @brief 全車両を描画する
	void render(const Array<Vehicle>& vehicles, Vec3 cameraPos);

	/// @brief 選択アウトライン用: 単一車両を単色で描画する
	void drawVehicleSilhouette(const Vehicle& v, Vec3 cameraPos, const ColorF& color);

private:
	/// @brief 単一車両を描画する（isClose で Model / Box を切り替え）
	void drawVehicle(const Vehicle& v, bool isClose);

	/// @brief 必要なら m_carModel を遅延ロードして返す
	Model& ensureCarModel();

	/// @brief 車両のワールド変換行列（Model 用: car.obj の X=前方, Y=上, Z=横）を計算する
	static Mat4x4 carModelWorldMatrix(const Vehicle& v);

	/// @brief isClose かつ乗用車/軽自動車なら OBJ モデルで描画するべきか
	static bool usesCarModel(const Vehicle& v, bool isClose);

	/// @brief 乗用車・軽自動車用 3D モデル（遅延ロード）
	Model m_carModel;
};
