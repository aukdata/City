#include "RailFacilities.hpp"
#include "RailStructure.hpp"

namespace RailFacilities
{
	namespace
	{
		void box(MeshData& mesh, const RailwaySite::Frame& frame, Vec3 center, Vec3 size)
		{
			RailStructure::prism(mesh,frame.point(0,0,center.z-size.z*.5),frame.point(0,0,center.z+size.z*.5),
				frame.right,frame.right,center.x,size.x*.5,center.y+size.y*.5,center.y-size.y*.5);
		}
		void gable(MeshData& mesh, const RailwaySite::Frame& frame, double left, double right, double start, double end, double eave, double ridge)
		{
			const double middle = (left+right)*.5;
			// 上面・下面・小口を閉じた2枚の屋根板にする。
			for (const auto span : {Vec2{left,middle},Vec2{middle,right}})
			{
				const double a = span.x == middle ? ridge : eave, b = span.y == middle ? ridge : eave;
				const Vec3 p = frame.point(span.x,a,start), q = frame.point(span.x,a,end), r = frame.point(span.y,b,end), s = frame.point(span.y,b,start);
				const Vec3 down{0,-.16,0};
				BridgeStructure::quad(mesh,p,q,r,s); BridgeStructure::quad(mesh,p+down,s+down,r+down,q+down);
				BridgeStructure::quad(mesh,p,p+down,q+down,q); BridgeStructure::quad(mesh,s,r,r+down,s+down);
				BridgeStructure::quad(mesh,p,s,s+down,p+down); BridgeStructure::quad(mesh,q,q+down,r+down,r);
			}
		}
		double groundAt(const World& world, Vec3 point) { return world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)); }
	}
	ColorF color(size_t material)
	{
		const std::array<ColorF,Count> palette={ColorF{.60,.59,.54},ColorF{.23,.34,.34},ColorF{.38,.42,.43},ColorF{.23,.38,.43},
			ColorF{.94,.71,.20},ColorF{.49,.33,.20},ColorF{.95,.93,.78},ColorF{.34,.35,.32}};
		return palette[material].removeSRGBCurve();
	}
	Geometry station(const TrainNetwork& network, const World& world, int stationId)
	{
		Geometry geometry; const auto* node = network.getNode(stationId);
		const auto frame = RailwaySite::stationFrame(network,stationId); if (!node || !frame) { return geometry; }
		const auto paths = RailwaySite::stationPaths(network,stationId);
		const bool urban = paths.size()>1;
		const bool underground = node->stationKind == StationKind::Underground;
		const bool terminal = node->stationKind == StationKind::Terminal;
		for (const auto& path : paths)
		{
			double distance = 0, nextColumn = 0, nextBench = 16;
			for (size_t i = 1; i < path.size(); ++i)
			{
				Vec3 along = path[i]-path[i-1]; along.y = 0; const double length = along.length(); if (length<.001) { continue; } along /= length;
				const RailwaySite::Frame local{path[i-1],along,{along.z,0,-along.x}};
				const Vec3 right = local.right;
				RailStructure::prism(geometry.parts[Concrete],path[i-1],path[i],right,right,-4,2.15,1.05,-.38);
				RailStructure::prism(geometry.parts[Tactile],path[i-1],path[i],right,right,-2.23,.17,1.066,1.05);
				// 転落防止柵をホーム外側だけに置き、車体側の空間を空ける。
				for (const double height : {1.60,2.13})
				{
					RailStructure::prism(geometry.parts[Steel],path[i-1],path[i],right,right,-6.02,.035,height+.035,height-.035);
				}
				box(geometry.parts[Steel],local,{-6.02,1.60,0},{.09,1.15,.09});
				// 地下ホームでは覆工自体が屋根。地上駅の庇を重ねるとアーチ外側へ食い込む。
				if (underground && distance >= nextColumn)
				{
					box(geometry.parts[Light], local, {-3.4, 3.6, 1}, {.22, .10, 1.45});
					nextColumn = distance + 12;
				}
				const bool covered = !underground && distance >= 4 && distance < (urban ? 78 : 48);
				if (covered)
				{
					RailStructure::prism(geometry.parts[Roof],path[i-1],path[i],right,right,-4.15,2.45,4.95,4.76);
					if (distance>=nextColumn)
					{
						box(geometry.parts[Steel],local,{-4.9,2.94,0},{.16,3.78,.16});
						box(geometry.parts[Steel],local,{-4.1,4.58,0},{4.6,.16,.18});
						box(geometry.parts[Light],local,{-3.4,4.58,1},{.22,.10,1.45}); nextColumn = distance+12;
					}
				}
				if (distance>=nextBench && distance<70)
				{
					box(geometry.parts[Timber],local,{-5.35,1.52,0},{.55,.12,2.4});
					box(geometry.parts[Timber],local,{-5.59,1.85,0},{.09,.55,2.4});
					for (const double z : {-.8,.8}) { box(geometry.parts[Steel],local,{-5.35,1.26,z},{.09,.45,.09}); }
					geometry.signs << Sign{local.point(-5.2,3.18,2.2),along,node->name,3.2};
					for (const double z : {.8,3.6}) { box(geometry.parts[Steel],local,{-5.2,2.1,z},{.075,2.2,.075}); }
					nextBench = distance+34;
				}
				const Vec3 column = local.point(-4.8,0,0); const double ground = groundAt(world,column)-local.origin.y;
				if (ground<-.8 && i%3==0) { box(geometry.parts[Concrete],local,{-4.8,(ground-.3)*.5,0},{.6,-ground-.3,.6}); }
				distance += length;
			}
		}
		const auto& base = *frame;
		if (underground)
		{
			// 地上は階段屋根とエレベーター棟だけ。ホームの駅舎を地上へ複製しない。
			const Vec3 entrance =
				node->entrance.value_or(base.point(-12, groundAt(world, base.origin) - base.origin.y, 8));
			const RailwaySite::Frame surface{entrance, base.along, base.right};
			for (const double x : {-2.5, 1.3})
			{
				box(geometry.parts[Concrete], surface, {x, -1.5, 0}, {.18, 3, 7.8});
			}
			box(geometry.parts[Concrete], surface, {-.6, -2.8, 3.3}, {3.6, .2, 1.1});
			box(geometry.parts[Concrete], surface, {-3.6, .12, 0}, {2.4, .24, 9.6});
			box(geometry.parts[Concrete], surface, {3.0, .12, 0}, {3.6, .24, 9.6});
			for (const double z : {-4.3, 4.3})
			{
				box(geometry.parts[Concrete], surface, {-.6, .12, z}, {3.6, .24, 1.0});
			}
			for (const double x : {-2.5, 1.3})
			{
				box(geometry.parts[Concrete], surface, {x, .4, 0}, {.18, .8, 7.8});
				box(geometry.parts[Glass], surface, {x, 1.75, 0}, {.07, 1.9, 7.8});
				for (const double z : {-3.7, 0.0, 3.7})
				{
					box(geometry.parts[Steel], surface, {x, 1.5, z}, {.09, 3, .09});
				}
			}
			box(geometry.parts[Roof], surface, {-.6, 3.1, 0}, {4.2, .18, 8.3});
			box(geometry.parts[Glass], surface, {3.2, 1.9, .6}, {2.4, 3.8, 3.8});
			box(geometry.parts[Steel], surface, {3.2, 3.85, .6}, {2.55, .16, 3.95});
			box(geometry.parts[Steel], surface, {3.2, 1.1, -1.33}, {1.1, 2.2, .05});
			geometry.signs << Sign{surface.point(-.6, 2.68, -3.98), surface.right, node->name + U"駅", 3.5};
			geometry.signs << Sign{surface.point(3.2, 3.1, -1.4), surface.right, U"EV", 1.5};
			// 開口した階段口と地下の中間踊り場。黒い立方体で入口を塞がない。
			for (int step = 0; step < 16; ++step)
			{
				box(geometry.parts[Concrete], surface, {-.6, -step * .18, -3.5 + step * .42}, {3.5, .18, .43});
			}
			box(geometry.parts[Light], surface, {-.6, 2.9, 0}, {.25, .1, 5.8});
			return geometry;
		}
		box(geometry.parts[Concrete],base,{-11, .45,14},{9,1.2,17});
		for (const double x : {-14.0,-8.0})
		{
			for (const double z : {8.0,20.0})
			{
				const double ground=groundAt(world,base.point(x,0,z))-base.origin.y;
				if (ground<-.15) { box(geometry.parts[Concrete],base,{x,(ground-.15)*.5,z},{.7,-ground-.15,.7}); }
			}
		}
		box(geometry.parts[urban ? Concrete : Timber],base,{-11,2.65,14},{7,3.2,14});
		if (!terminal)
		{
			gable(geometry.parts[Roof], base, -15.3, -6.7, 5.8, 22.2, 4.35, 5.65);
		}
		else
		{
			const auto& settings = GenerationSettings::get();
			const double width = settings.landmarks_terminalWidth, length = settings.landmarks_terminalLength;
			// 線路上の橋上コンコース。柱はホーム上に置き、車両限界を空ける。
			box(geometry.parts[Concrete], base, {0, 7.0, length * .5 + 3}, {width, .45, length});
			box(geometry.parts[Roof], base, {0, 11.6, length * .5 + 3}, {width + 1, .28, length + 1});
			for (const double x : {-width * .5, width * .5})
			{
				box(geometry.parts[Glass], base, {x, 9.35, length * .5 + 3}, {.12, 4.3, length});
				for (double z = 4; z < length + 3; z += 6)
				{
					box(geometry.parts[Steel], base, {x, 9.35, z}, {.22, 4.5, .22});
				}
			}
			for (const double z : {3.0, length + 3})
			{
				box(geometry.parts[Glass], base, {0, 9.35, z}, {width, 4.3, .12});
			}
			for (const double x : {-7.4, 7.4})
			{
				for (double z = 8; z < length; z += 12)
				{
					box(geometry.parts[Concrete], base, {x, 4.0, z}, {.55, 6, .55});
				}
			}
			// 改札機、ホーム連絡エレベーター、タクシー・バス待合を一体化。
			for (const double x : {-4.0, -2.0, 0.0, 2.0, 4.0})
			{
				box(geometry.parts[Steel], base, {x, 7.8, 9}, {.5, 1.1, 1.8});
			}
			for (const double x : {-6.9, 6.9})
			{
				box(geometry.parts[Glass], base, {x, 4.1, 20}, {2.2, 6.2, 2.5});
			}
			// バス・タクシーの待合はホーム標高でなく、実際の地表へ下ろす。
			const double forecourtGround=groundAt(world,base.point(-12,0,38))-base.origin.y;
			box(geometry.parts[Concrete],base,{-12,forecourtGround+.03,38},{6,.06,12});
			box(geometry.parts[Roof], base, {-12, forecourtGround+3.0, 38}, {5.5, .15, 10});
			for (const double z : {34.0, 42.0})
			{
				box(geometry.parts[Steel], base, {-14, forecourtGround+1.5, z}, {.12, 3.0, .12});
			}
			geometry.signs << Sign{base.point(0, 10.1, 2.85), base.right, node->name + U" ターミナル", width * .7};
			geometry.signs << Sign{base.point(-12, forecourtGround+2.5, 33), base.right, U"バス・タクシー", 5};
		}
		for (const double z : {9.0,14.0,19.0})
		{
			box(geometry.parts[Steel],base,{-7.46,2.75,z},{.10,1.65,2.8});
			box(geometry.parts[Glass],base,{-7.39,2.75,z},{.035,1.43,2.56});
			box(geometry.parts[Steel],base,{-7.35,2.75,z},{.025,1.5,.05});
		}
		box(geometry.parts[Concrete],base,{-6.7,1.0,14},{1.7,.10,5});
		box(geometry.parts[Roof],base,{-6.1,3.9,14},{3,.16,5.4});
		geometry.signs << Sign{base.point(-7.30,3.85,14),base.along,node->name+U"駅",4.8};
		// 地面から駅舎へ連続する階段。高架駅でも入口を浮かせない。
		const double ground = groundAt(world,base.point(-12,0,40))-base.origin.y;
		const double rise = Max(.18,1.05-ground); const int steps = Max(1,static_cast<int>(std::ceil(rise/.18)));
		for (int step = 0; step < steps; ++step)
		{
			const double top = 1.05-rise*step/steps;
			box(geometry.parts[Concrete],base,{-12,(top+ground)*.5,22.3+step*.28},{2.4,Max(.05,top-ground),.29});
		}
		for (const double x : {-13.3,-10.7})
		{
			RailStructure::prism(geometry.parts[Steel],base.point(x,1.05,22.1),base.point(x,ground,22.3+steps*.28),
				base.right,base.right,0,.035,.96,.90);
		}
		return geometry;
	}
	Geometry depot(const TrainNetwork& network, const World& world, const RailDepot& depot)
	{
		Geometry geometry; const auto frame = RailwaySite::depotFrame(network,depot); if (!frame) { return geometry; }
		const auto& base = *frame;
		box(geometry.parts[Ballast],base,{3.5,-.38,66},{18,.65,140});
		for (const double x : {-4.8,11.8})
		{
			for (int z = 0; z <= 132; z += 12)
			{
				box(geometry.parts[Steel],base,{x,1.0,static_cast<double>(z)},{.08,2.0,.08});
				const double ground = groundAt(world,base.point(x,0,z))-base.origin.y;
				if (ground<-.7) { box(geometry.parts[Concrete],base,{x,(ground-.7)*.5,static_cast<double>(z)},{.8,-ground-.7,.8}); }
			}
			for (const double height : {.65,1.35,1.96}) { box(geometry.parts[Steel],base,{x,height,66},{.045,.045,132}); }
		}
		// 2線とも間口を開け、車体の位置にシャッターや柱を置かない。
		for (const double x : {-4.2,3.5,11.2})
		{
			for (const double z : {35.0,55.0,75.0,95.0,115.0}) { box(geometry.parts[Steel],base,{x,2.9,z},{.25,5.8,.25}); }
		}
		for (const double x : {-4.35,11.35})
		{
			box(geometry.parts[Concrete],base,{x,1.3,75},{.16,2.6,80});
			box(geometry.parts[Glass],base,{x,3.5,75},{.10,1.5,79});
			box(geometry.parts[Roof],base,{x,5.0,75},{.16,1.4,80});
		}
		for (const Vec2 span : {Vec2{-4.6,3.5},Vec2{3.5,11.6}}) { gable(geometry.parts[Roof],base,span.x,span.y,34,116,5.9,6.9); }
		for (const double z : {35.0,55.0,75.0,95.0,115.0})
		{
			box(geometry.parts[Steel],base,{3.5,5.7,z},{15.5,.25,.25});
		}
		for (const double x : {0.0,7.0})
		{
			box(geometry.parts[Steel],base,{x,.6,129},{2.2,.18,.18});
			for (const double offset : {-.7,.7}) { box(geometry.parts[Steel],base,{x+offset,.3,129},{.12,.6,.18}); }
		}
		geometry.signs << Sign{base.point(-4.48,4.7,52),base.along,depot.name,6.0};
		return geometry;
	}
	Array<Train> parkedTrains(const TrainNetwork& network)
	{
		Array<Train> result;
		for (const auto& depot : network.depots())
		{
			for (size_t siding = 0; siding < depot.sidingNodes.size(); ++siding)
			{
				const auto route = network.findRoute(depot.throatNodeId,depot.sidingNodes[siding]); if (route.size()!=1) { continue; }
				const auto* edge = network.getEdge(route.front()); if (!edge || edge->length<120) { continue; }
				Train train; train.type = siding==0 ? TrainType::Local : TrainType::Express;
				train.currentEdge = edge->id; train.routeEdges = route; train.forward = edge->nodeA == depot.throatNodeId;
				train.arcPos = 108; train.state = TrainState::OutOfService;
				result << std::move(train);
			}
		}
		return result;
	}
}
