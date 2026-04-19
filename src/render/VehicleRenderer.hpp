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

	/// @brief 乗用車・軽自動車用 3D モデル（遅延ロード）
	Model m_carModel;
};
