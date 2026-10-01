#include "VehicleRenderer.hpp"
#include "ShaderAsset.hpp"
#include "VehiclePaint.hpp"
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
	return isClose && (v.type == VehicleType::PassengerCar || v.type == VehicleType::KeiCar
		|| v.type == VehicleType::Bus || v.type == VehicleType::Emergency
		|| v.type == VehicleType::SmallTruck || v.type == VehicleType::LargeTruck);
}

String VehicleRenderer::modelStem(const Vehicle& v)
{
	String stem = U"sedan";
	if (v.type == VehicleType::KeiCar)
	{
		stem = U"kei_wagon";
	}
	else if (v.type == VehicleType::Bus)
	{
		stem = U"city_bus";
	}
	else if (v.type == VehicleType::SmallTruck)
	{
		stem = U"delivery_truck";
	}
	else if (v.type == VehicleType::LargeTruck)
	{
		stem = U"cargo_truck";
	}
	else if (v.type == VehicleType::Emergency)
	{
		stem = ((v.id & 1) == 0) ? U"patrol_car" : U"fire_engine";
	}
	return stem;
}

Model& VehicleRenderer::ensureVehicleModel(const Vehicle& v, int level)
{
	const String stem = modelStem(v);
	auto [it, inserted] = m_vehicleModels.try_emplace(stem, U"assets/vehicles/{}.obj"_fmt(stem));
	return it->second.at(level);
}

void VehicleRenderer::render(const Array<Vehicle>& vehicles, Vec3 cameraPos, const BasicCamera3D* camera)
{
	m_drawCalls = m_submitted = 0;
	for (auto& asset : m_farAssets) { for (auto& batch : asset.batches) { batch.clear(); } }
	Optional<ViewFrustum> frustum;
	if (camera) { frustum = ViewFrustum{*camera, 6000}; }
	constexpr double kClose = 100, kMedium = 350, kMaximum = 5000;
	for (const auto& vehicle : vehicles)
	{
		const double distanceSq = vehicle.position.distanceFromSq(cameraPos);
		if (distanceSq > kMaximum * kMaximum) { continue; }
		if (frustum && !frustum->intersects(Sphere{vehicle.position + Vec3{0,2,0}, 8})) { continue; }
		++m_submitted;
		const int level = distanceSq < kClose * kClose ? 0 : (distanceSq < kMedium * kMedium ? 1 : 2);
		if (level < 2 || !usesCarModel(vehicle, true)) { drawVehicle(vehicle, level); continue; }
		const size_t index = vehicle.type == VehicleType::Emergency ? (vehicle.id & 1 ? 8 : 7) : static_cast<size_t>(vehicle.type);
		auto& asset = m_farAssets[index];
		if (asset.parts.isEmpty())
		{
			const auto path = modelLodPath(U"assets/vehicles/{}.obj"_fmt(modelStem(vehicle)), 2);
			asset.parts = loadModelMeshSource(path, ensureVehicleModel(vehicle, 2));
			asset.batches.resize(asset.parts.size());
		}
		const auto transform = carModelWorldMatrix(vehicle);
		for (size_t part=0;part<asset.parts.size();++part)
		{
			Float4 tint{1,1,1,1};
			if ((vehicle.type==VehicleType::PassengerCar || vehicle.type==VehicleType::KeiCar) && asset.parts[part].material.name.starts_with(U"body"))
			{
				const auto paint=VehiclePaint::color(vehicle);const auto base=asset.parts[part].material.diffuse;
				tint=Float4{static_cast<float>(paint.r/Max(.001,base.r)),static_cast<float>(paint.g/Max(.001,base.g)),static_cast<float>(paint.b/Max(.001,base.b)),1};
			}
			asset.batches[part].append(asset.parts[part],transform,tint);
		}
	}
	for (auto& asset : m_farAssets)
	{
		for (auto& batch : asset.batches)
		{
			if (batch.triangles() == 0) { continue; }
			m_drawCalls += batch.draw();
		}
	}
}

void VehicleRenderer::renderShadowCasters(const Array<Vehicle>& vehicles, Vec3 focus, double radius)
{
	for (const Vehicle& vehicle : vehicles)
	{
		const Vec2 delta{ vehicle.position.x - focus.x, vehicle.position.z - focus.z };
		if (delta.lengthSq() <= radius * radius)
		{
			drawVehicleSilhouette(vehicle, focus, ColorF{ 1 });
		}
	}
}

void VehicleRenderer::drawVehicleSilhouette(const Vehicle& v, Vec3 cameraPos, const ColorF& color)
{
	// シルエット描画でも通常描画と同じ LOD 判定を使い、選択アウトラインの見え方を揃える。
	const int level = v.position.distanceFromSq(cameraPos) < 100 * 100 ? 0 : 1;

	if (usesCarModel(v, true))
	{
		Model& model = ensureVehicleModel(v, level);
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

void VehicleRenderer::drawVehicle(const Vehicle& v, int level)
{
	// 対応車種は近距離で詳細モデルを描画し、遠景では既存の簡易表示を使う。
	if (usesCarModel(v, true))
	{
		Model& model = ensureVehicleModel(v, level);
		const auto& materials = model.materials();
		Optional<ScopedCustomShader3D> paintScope;
		if (v.type==VehicleType::PassengerCar || v.type==VehicleType::KeiCar)
		{
			static const PixelShader shader{ShaderAsset::pixel(U"shaders/hlsl/city_forward.hlsl", U"VehiclePaint_PS")};
			m_paint->color=VehiclePaint::color(v).toFloat4();
			Graphics3D::SetPSConstantBuffer(5,m_paint);paintScope.emplace(shader);
		}
		for (const auto& obj : model.objects())
		{
			const Transformer3D transform{ carModelWorldMatrix(v) };
			obj.draw(materials);
			m_drawCalls += obj.parts.size();
		}
		return;
	}

	const auto vis = getVehicleVisual(v.type);
	const Vec3 center = v.position + Vec3{ 0, vis.size.y / 2, 0 };
	// OrientedBox: Z=前方 → RotateX(-pitch) で傾斜, RotateY で yaw
	OrientedBox{ center, vis.size, boxWorldRotation(v) }.draw(vis.color.removeSRGBCurve());
	++m_drawCalls;
}
