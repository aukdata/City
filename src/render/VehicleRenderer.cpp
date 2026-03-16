#include "VehicleRenderer.hpp"

void VehicleRenderer::render(const Array<Vehicle>& vehicles)
{
	for (const auto& v : vehicles)
	{
		drawVehicle(v);
	}
}

void VehicleRenderer::drawVehicle(const Vehicle& v)
{
	const ColorF color = vehicleColor(v.type);
	const Vec3 size = vehicleSize(v.type);

	Box{ v.position + Vec3{ 0, size.y / 2, 0 }, size }.draw(color);
}

ColorF VehicleRenderer::vehicleColor(VehicleType type)
{
	switch (type)
	{
	case VehicleType::PassengerCar: return ColorF{ 0.8, 0.2, 0.2 };
	case VehicleType::KeiCar:       return ColorF{ 0.2, 0.6, 0.8 };
	case VehicleType::Bus:          return ColorF{ 0.9, 0.7, 0.1 };
	case VehicleType::LargeTruck:   return ColorF{ 0.5, 0.4, 0.3 };
	case VehicleType::Emergency:    return ColorF{ 1.0, 0.0, 0.0 };
	default:                        return ColorF{ 0.6, 0.6, 0.6 };
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
