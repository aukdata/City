#pragma once
#include "../traffic/Vehicle.hpp"

/// @brief 車両の描画クラス
class VehicleRenderer
{
public:
	/// @brief 全車両を描画する
	void render(const Array<Vehicle>& vehicles);

private:
	/// @brief 単一車両を描画する
	void drawVehicle(const Vehicle& v);

	/// @brief 車種別の車体色を返す
	static ColorF vehicleColor(VehicleType type);

	/// @brief 車種別の車体サイズ (width, height, length) を返す
	static Vec3 vehicleSize(VehicleType type);

	/// @brief 乗用車・軽自動車用 3D モデル（遅延ロード）
	Model m_carModel;
};
