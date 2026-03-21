#include "VehicleRenderer.hpp"
#include "RoadRenderer.hpp"   // kLodDistSq

void VehicleRenderer::render(const Array<Vehicle>& vehicles, Vec3 cameraPos)
{
	for (const auto& v : vehicles)
	{
		const double dx = v.position.x - cameraPos.x;
		const double dz = v.position.z - cameraPos.z;
		const bool isClose = (dx * dx + dz * dz) < RoadRenderer::kLodDistSq;
		drawVehicle(v, isClose);
	}
}

void VehicleRenderer::drawVehicle(const Vehicle& v, bool isClose)
{
	// 近距離の乗用車・軽自動車は car.obj モデルで描画する
	if (isClose && (v.type == VehicleType::PassengerCar || v.type == VehicleType::KeiCar))
	{
		// 初回のみモデルとテクスチャをロードする
		if (m_carModel.isEmpty())
		{
			m_carModel = Model{ U"assets/models/car.obj" };
			Model::RegisterDiffuseTextures(m_carModel, TextureDesc::MippedSRGB);
		}

		// heading = atan2(tx, tz)（+Z 基準）、モデルは +X を向いているため -π/2 補正
		// Transformer3D + obj.draw(materials) で確実にワールド空間へ配置する（tutorial 37.2 方式）
		{
			const Mat4x4 worldMat = Mat4x4::RotateY(
				v.heading - static_cast<float>(Math::HalfPi))
				.translated(
					static_cast<float>(v.position.x),
					static_cast<float>(v.position.y),
					static_cast<float>(v.position.z));
			const auto& materials = m_carModel.materials();
			for (const auto& obj : m_carModel.objects())
			{
				const Transformer3D transform{ worldMat };
				obj.draw(materials);
			}
		}
		return;
	}

	// 遠距離、またはモデルのない車種はボックスで描画する
	const ColorF    color  = vehicleColor(v.type);
	const Vec3      size   = vehicleSize(v.type);
	const Vec3      center = v.position + Vec3{ 0, size.y / 2, 0 };
	const Quaternion rot   = Quaternion::RotateY(v.heading);
	OrientedBox{ center, size, rot }.draw(color);
}

ColorF VehicleRenderer::vehicleColor(VehicleType type)
{
	switch (type)
	{
	case VehicleType::PassengerCar: return ColorF{ 0.8, 0.2, 0.2 }.removeSRGBCurve();
	case VehicleType::KeiCar:       return ColorF{ 0.2, 0.6, 0.8 }.removeSRGBCurve();
	case VehicleType::Bus:          return ColorF{ 0.9, 0.7, 0.1 }.removeSRGBCurve();
	case VehicleType::LargeTruck:   return ColorF{ 0.5, 0.4, 0.3 }.removeSRGBCurve();
	case VehicleType::Emergency:    return ColorF{ 1.0, 0.0, 0.0 }.removeSRGBCurve();
	default:                        return ColorF{ 0.6, 0.6, 0.6 }.removeSRGBCurve();
	}
}

Vec3 VehicleRenderer::vehicleSize(VehicleType type)
{
	switch (type)
	{
	case VehicleType::PassengerCar: return Vec3{ 1.8, 1.4, 4.5 };
	case VehicleType::KeiCar:       return Vec3{ 1.5, 1.6, 3.4 };
	case VehicleType::Bus:          return Vec3{ 2.5, 3.0, 12.0 };
	case VehicleType::LargeTruck:   return Vec3{ 2.5, 3.5, 10.0 };
	case VehicleType::SmallTruck:   return Vec3{ 2.0, 2.5, 7.0 };
	case VehicleType::Emergency:    return Vec3{ 2.0, 2.0, 5.0 };
	default:                        return Vec3{ 1.8, 1.5, 4.0 };
	}
}
