#pragma once
#include "Train.hpp"
#include "TrainNetwork.hpp"

/// @brief 編成寸法と線路上の車体位置。描画・占有・生成で共用する。
namespace TrainConsist
{
	struct Profile { int cars; float carSpacing; float maximumSpeed; StringView cab; StringView trailer; };
	inline Profile profile(TrainType type)
	{
		return type == TrainType::Local ? Profile{2,20,110,U"regional_cab",U"regional_trailer"}
			: Profile{4,20,120,U"urban_cab",U"urban_trailer"};
	}
	inline float length(TrainType type) { const auto value=profile(type); return value.cars*value.carSpacing; }
	/// @brief 先頭から後方への距離を、逆向き区間を含めて線路上に投影する。
	inline Optional<Vec3> behind(const Train& train,const TrainNetwork& network,float offset)
	{
		int progress=train.routeProgress;
		bool forward=train.forward;
		float arc=train.arcPos-offset;
		while (progress>=0 && progress<static_cast<int>(train.routeEdges.size()))
		{
			const int id=train.routeEdges[progress];
			const auto* edge=network.getEdge(id);
			const auto curve=network.getBezier(id);
			if (!edge || !curve) { return none; }
			if (arc>=0)
			{
				const int lane = TransportCrossSection::railLane(*edge,forward); if (lane < 0) { return none; }
				return TransportCrossSection::lanePosition(*edge,*curve,forward ? Min(arc,edge->length) : Max(0.0f,edge->length-arc),lane);
			}
			const int start=forward ? edge->nodeA : edge->nodeB;
			if (--progress<0) { return none; }
			const auto* previous=network.getEdge(train.routeEdges[progress]);
			if (!previous || (previous->nodeA!=start && previous->nodeB!=start)) { return none; }
			forward=previous->nodeB==start;
			arc+=previous->length;
		}
		return none;
	}
	struct CarPose
	{
		Vec3 position, forward, right, up;
		Mat4x4 transform;
		String model;
	};
	/// @brief 台車の位置から求めた同一の車体姿勢を描画と選択で使用する。
	inline Optional<CarPose> carPose(const Train& train,const TrainNetwork& network,int car)
	{
		const auto spec=profile(train.type);
		const float center=(car+.5f)*spec.carSpacing;
		const auto front=behind(train,network,center-6.75f),rear=behind(train,network,center+6.75f);
		if (!front || !rear || front->distanceFromSq(*rear)<1e-6) { return none; }
		const Vec3 direction=(*front-*rear).normalized();
		const Vec3 right=tangentToRight(direction),up=direction.cross(right);
		const double heading=Atan2(direction.x,direction.z),pitch=-Atan2(direction.y,Vec2{direction.x,direction.z}.length());
		const bool last=car+1==spec.cars;
		const auto* edge = network.getEdge(train.currentEdge);
		const Vec3 position=(*front+*rear)*.5+Vec3{0,edge ? TransportCrossSection::railTop(*edge) : .17,0};
		return CarPose{position,direction,right,up,Mat4x4::RotateY(last ? Math::Pi : 0)*Mat4x4::RotateX(pitch)*Mat4x4::RotateY(heading)*Mat4x4::Translate(position),String{car==0 || last ? spec.cab : spec.trailer}};
	}
	/// @brief 傾斜した車体座標系へレイを投影し、編成中の最も近い車両を選ぶ。
	inline Optional<double> hitDistance(
		const Train& train, const TrainNetwork& network, const Ray& ray, const std::function<bool(Vec3)>& visible = {})
	{
		Optional<double> nearest;
		for (int car=0;car<profile(train.type).cars;++car)
		{
			const auto pose = carPose(train, network, car);
			if (!pose || (visible && !visible(pose->position)))
			{
				continue;
			}
			const Vec3 origin=Vec3{ray.origin.xyz()}-pose->position;
			const Vec3 direction{ray.direction.xyz()};
			const Vec3 localOrigin{origin.dot(pose->right),origin.dot(pose->up),origin.dot(pose->forward)};
			const Vec3 localDirection{direction.dot(pose->right),direction.dot(pose->up),direction.dot(pose->forward)};
			const Ray local{localOrigin,localDirection};
			if (const auto distance=Box{Vec3{0,1.85,0},Vec3{3.1,3.7,19.6}}.intersects(local))
			{
				if (!nearest || *distance<*nearest) { nearest=*distance; }
			}
		}
		return nearest;
	}

}
