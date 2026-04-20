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

	/// @brief OrientedBox 用のワールド変換 Quaternion（Z=前方）
	Quaternion boxWorldRotation(const Vehicle& v)
	{
		return Quaternion::RotateX(-v.pitch) * Quaternion::RotateY(v.heading);
	}
}

Mat4x4 VehicleRenderer::carModelWorldMatrix(const Vehicle& v)
{
	// car.obj: X=前方, Y=上, Z=横 → RotateZ で pitch, RotateY で yaw
	return (Mat4x4::RotateZ(v.pitch)
		* Mat4x4::RotateY(v.heading - static_cast<float>(Math::HalfPi)))
		.translated(
			static_cast<float>(v.position.x),
			static_cast<float>(v.position.y),
			static_cast<float>(v.position.z));
}

bool VehicleRenderer::usesCarModel(const Vehicle& v, bool isClose)
{
	return isClose
		&& (v.type == VehicleType::PassengerCar || v.type == VehicleType::KeiCar);
}

Model& VehicleRenderer::ensureCarModel()
{
	if (m_carModel.isEmpty())
	{
		m_carModel = Model{ U"assets/vehicles/car.obj" };
		Model::RegisterDiffuseTextures(m_carModel, TextureDesc::MippedSRGB);
	}
	return m_carModel;
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

void VehicleRenderer::drawVehicleSilhouette(const Vehicle& v, Vec3 cameraPos, const ColorF& color)
{
	const double dx = v.position.x - cameraPos.x;
	const double dz = v.position.z - cameraPos.z;
	const bool isClose = (dx * dx + dz * dz) < RoadRenderer::kLodDistSq;

	if (usesCarModel(v, isClose))
	{
		Model& model = ensureCarModel();
		const Transformer3D transform{ carModelWorldMatrix(v) };
		for (const auto& obj : model.objects())
		{
			for (const auto& part : obj.parts)
				part.mesh.draw(color);
		}
		return;
	}

	const auto vis = getVehicleVisual(v.type);
	const Vec3 center = v.position + Vec3{ 0, vis.size.y / 2, 0 };
	OrientedBox{ center, vis.size, boxWorldRotation(v) }.draw(color);
}

void VehicleRenderer::drawVehicle(const Vehicle& v, bool isClose)
{
	if (usesCarModel(v, isClose))
	{
		Model& model = ensureCarModel();
		const auto& materials = model.materials();
		for (const auto& obj : model.objects())
		{
			const Transformer3D transform{ carModelWorldMatrix(v) };
			obj.draw(materials);
		}
		return;
	}

	const auto vis = getVehicleVisual(v.type);
	const Vec3 center = v.position + Vec3{ 0, vis.size.y / 2, 0 };
	// OrientedBox: Z=前方 → RotateX(-pitch) で傾斜, RotateY で yaw
	OrientedBox{ center, vis.size, boxWorldRotation(v) }.draw(vis.color.removeSRGBCurve());
}
