#include "VehicleRenderer.hpp"
#include "RoadRenderer.hpp"   // kLodDistSq

namespace
{
	/// @brief 車種ごとの描画属性（色とサイズを一元管理）
	struct VehicleVisual
	{
		ColorF color;
		Vec3   size;   ///< (width, height, length)
	};

	VehicleVisual getVehicleVisual(VehicleType type)
	{
		switch (type)
		{
		case VehicleType::PassengerCar: return { ColorF{ 0.8, 0.2, 0.2 }, { 1.8, 1.4, 4.5 } };
		case VehicleType::KeiCar:       return { ColorF{ 0.2, 0.6, 0.8 }, { 1.5, 1.6, 3.4 } };
		case VehicleType::Bus:          return { ColorF{ 0.9, 0.7, 0.1 }, { 2.5, 3.0, 12.0 } };
		case VehicleType::SmallTruck:   return { ColorF{ 0.5, 0.5, 0.4 }, { 2.0, 2.5, 7.0 } };
		case VehicleType::LargeTruck:   return { ColorF{ 0.5, 0.4, 0.3 }, { 2.5, 3.5, 10.0 } };
		case VehicleType::Emergency:    return { ColorF{ 1.0, 0.0, 0.0 }, { 2.0, 2.0, 5.0 } };
		default:                        return { ColorF{ 0.6, 0.6, 0.6 }, { 1.8, 1.5, 4.0 } };
		}
	}
}

void VehicleRenderer::render(const Array<Vehicle>& vehicles, Vec3 cameraPos)
{
	for (const auto& v : vehicles)
	{
		const double dx = v.position.x - cameraPos.x;
		const double dz = v.position.z - cameraPos.z;
		const bool isClose = (dx * dx + dz * dz) < RoadRenderer::kLodDistSq;
		drawVehicle(v, isClose);

		const Vec3 markerPos = v.position + Vec3{ 0, 60, 0 };
		Sphere{ markerPos, 6.25 }.draw(ColorF{ 1.0, 0.3, 0.1, 0.5 }.removeSRGBCurve());
	}
}

void VehicleRenderer::drawVehicle(const Vehicle& v, bool isClose)
{
	if (isClose && (v.type == VehicleType::PassengerCar || v.type == VehicleType::KeiCar))
	{
		if (m_carModel.isEmpty())
		{
			m_carModel = Model{ U"assets/vehicles/car.obj" };
			Model::RegisterDiffuseTextures(m_carModel, TextureDesc::MippedSRGB);
		}

		{
			// car.obj: X=前方, Y=上, Z=横 → RotateZ で pitch, RotateY で yaw
			const Mat4x4 worldMat = (Mat4x4::RotateZ(v.pitch)
				* Mat4x4::RotateY(v.heading - static_cast<float>(Math::HalfPi)))
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

	const auto vis = getVehicleVisual(v.type);
	const Vec3 center = v.position + Vec3{ 0, vis.size.y / 2, 0 };
	// OrientedBox: Z=前方 → RotateX(-pitch) で傾斜, RotateY で yaw
	const Quaternion rot = Quaternion::RotateX(-v.pitch) * Quaternion::RotateY(v.heading);
	OrientedBox{ center, vis.size, rot }.draw(vis.color.removeSRGBCurve());
}
