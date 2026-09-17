#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/TransportCrossSection.hpp"
#include "src/road/RoadPreset.hpp"
#include "src/railway/TrainNetwork.hpp"
#include "src/railway/TrainManager.hpp"
#include "src/railway/TrainConsist.hpp"
#include "src/railway/RailTimetable.hpp"
#include "src/traffic/TrafficGraph.hpp"
#include "src/traffic/TrafficSpawn.hpp"
#include "src/traffic/DrivingController.hpp"
#include "src/save/RoadBinary.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/render/TrainRenderer.hpp"
#include "src/ui/TransportSectionControls.hpp"
#include "src/ui/RoadPlanToolbar.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace
{
	/// @brief 道路と鉄道が同じエッジを使う検証用の複線。
	int doubleTrack(RoadNetwork& roads, TrainNetwork& trains, bool street = false)
	{
		trains.bind(&roads);
		const int a = trains.addStation({512,20,200},U"北町"), b = trains.addStation({512,20,900},U"南町");
		const int id = trains.addEdge(a,b,{512,20,430},{512,20,670},80,true);
		if (street) { TransportCrossSection::railway(*roads.getEdge(id),true,false,true); }
		return id;
	}
}

void registerSharedTransportTests(TestRunner& runner)
{
	runner.add(U"SharedTransport.GeometryAndTravelPermissions",[](TestContext& context)
	{
		RoadNetwork roads; TrainNetwork trains; const int id = doubleTrack(roads,trains,true);
		auto* edge = roads.getEdge(id);
		context.expect(trains.getEdge(id) == edge,U"鉄道と道路は同じ路盤・断面の実体を参照する");
		edge->ctrlA.x += 5;
		context.expect(trains.getBezier(id)->p1 == roads.getBezier(id)->p1,U"共通網の編集は列車の線形にも直ちに反映される");
		const SimGraph sim=SimGraph::build(roads);
		TrafficGraph traffic; traffic.rebuild(sim,0,{});
		for (int i=0;i<static_cast<int>(edge->lanes.size());++i)
		{
			const bool rail = edge->lanes[i].type == LaneType::Rail;
			context.expect(TrafficSpawn::laneOpen(*sim.getEdge(id),i)==!rail,U"自動出現も軌道を選ばない");
			context.expect((traffic.entryNodeId(id,i)<0)==rail,U"自動車の探索グラフは軌道を含まない");
			context.expect(isPassable(*edge,i,TransportMode::Rail)==rail,U"列車の走行資格は軌道に限定される");
			context.expect(isPassable(*edge,i)==!rail,U"自動車の走行資格は車線に限定される");
		}
		const int origin = edge->nodeA, destination = edge->nodeB;
		const int isolated = roads.addNode({800,20,950});
		const int roadOnly = *roads.addEdge(destination,isolated,{610,20,920},{720,20,940});
		roads.getEdge(roadOnly)->edgeState=EdgeState::Existing; trains.synchronize();
		context.expect(!trains.getEdge(roadOnly) && trains.findRoute(origin,isolated).isEmpty(),U"道路しかない区間へ列車の経路を延長しない");
	});
	runner.add(U"SharedTransport.OpposingTrainsUseDifferentLanes",[](TestContext& context)
	{
		RoadNetwork roads; TrainNetwork trains; const int id=doubleTrack(roads,trains);
		auto* edge=trains.getEdge(id);
		context.expect(trains.tryOccupy(id,10,true) && trains.tryOccupy(id,11,false),U"上下線は異なる列車が同時に占有できる");
		context.expect(!trains.tryOccupy(id,12,true),U"同じ軌道の二重占有は禁止する");
		trains.releaseOccupy(id,10); trains.releaseOccupy(id,11);
		const int backwardLane = TransportCrossSection::railLane(*edge,false);
		edge->lanes[backwardLane].op=OpState::Closed;
		context.expect(!RailTimetable::validate(trains,RailTimetable::makeDefault(trains,edge->nodeA,edge->nodeB)).isEmpty(),U"復路の軌道が閉鎖されたダイヤは受け付けない");
		edge->lanes[backwardLane].op=OpState::Open;
		TrainManager manager; manager.init(&trains);
		auto forward=RailTimetable::makeDefault(trains,edge->nodeA,edge->nodeB);
		auto backward=RailTimetable::makeDefault(trains,edge->nodeB,edge->nodeA);
		trains.addSchedule(forward); trains.addSchedule(backward); manager.update(.1,0);
		context.expectEqual(manager.trains().size(),size_t{2},U"複線では上下の定期列車が同時に発車する");
		if (manager.trains().size()!=2) { return; }
		const auto& a=manager.trains()[0]; const auto& b=manager.trains()[1];
		context.expectNear(Abs(a.position.x-b.position.x),TransportCrossSection::kTrackSpacing,.001,U"対向する編成は軌道間隔を保つ");
		for (const auto& train:manager.trains())
		{
			const auto rear=TrainConsist::behind(train,trains,30);
			context.expect(rear.has_value(),U"後部車両も同じ軌道をたどる");
			if(rear) { context.expectNear(rear->x,train.position.x,.001,U"編成途中で路盤中心へ戻らない"); }
		}
	});
	runner.add(U"SharedTransport.DirectionalReturnRoute",[](TestContext& context)
	{
		TrainNetwork network;
		const int a=network.addStation({0,20,0},U"A"), b=network.addStation({0,20,800},U"B"), c=network.addNode({400,20,400});
		const int outward=network.addEdge(a,b,{0,20,260},{0,20,540});
		const int returnFirst=network.addEdge(b,c,{140,20,680},{300,20,500});
		const int returnLast=network.addEdge(c,a,{300,20,280},{140,20,120});
		for (const int id : {outward,returnFirst,returnLast}) { network.getEdge(id)->lanes.front().bidirectional=false; }
		const auto schedule=RailTimetable::makeDefault(network,a,b);
		context.expect(RailTimetable::validate(network,schedule).isEmpty(),U"往復で異なる軌道を使う路線も運行できる");
		context.expect(RailTimetable::route(network,schedule)==Array<int>{outward},U"往路は順方向の軌道を使う");
		context.expect(RailTimetable::route(network,schedule,true)==Array<int>{returnFirst,returnLast},U"復路を往路の逆走で代用しない");
	});
	runner.add(U"SharedTransport.SharedAndStandaloneStorage",[](TestContext& context)
	{
		RoadNetwork roads; TrainNetwork trains; const int id=doubleTrack(roads,trains,true);
		trains.addSchedule(RailTimetable::makeDefault(trains,0,1));
		const JSON state=trains.saveState();
		context.expect(!state[U"edges"][0].hasElement(U"ctrlA"),U"本体の線形を鉄道メタデータへ重複保存しない");
		context.expect(RoadBinary::writeGlobal(U"TestResults/shared_transport.bin",roads),U"共通断面を保存できる");
		RoadNetwork loaded;
		context.expect(RoadBinary::readGlobal(U"TestResults/shared_transport.bin",loaded),U"共通断面を復元できる");
		TrainNetwork restored; restored.bind(&loaded);
		context.expect(restored.restoreState(state),U"ダイヤは復元した道路網の軌道へ接続する");
		context.expect(restored.getEdge(id)==loaded.getEdge(id),U"ロード後も路盤の実体は一つ");
		TrainNetwork single; const int a=single.addStation({0,20,0},U"A"),b=single.addStation({0,20,400},U"B");
		const int track=single.addEdge(a,b,{0,20,130},{0,20,270});
		TransportCrossSection::railway(*single.getEdge(track),false,true);
		TrainNetwork copy; context.expect(copy.restoreState(single.saveState()),U"単体のスラブ単線も共通断面形式で復元できる");
		context.expect(copy.getEdge(track)->lanes.front().bidirectional && copy.getEdge(track)->parts.front().defId==U"roadbed_slab",U"材質と両方向単線の運用を保持する");
	});
	runner.add(U"SharedTransport.EditingKeepsInfrastructureConsistent",[](TestContext& context)
	{
		RoadNetwork roads; TrainNetwork trains; const int id=doubleTrack(roads,trains);
		const int origin=trains.getEdge(id)->nodeA, destination=trains.getEdge(id)->nodeB;
		trains.addSchedule(RailTimetable::makeDefault(trains,origin,destination));
		trains.tryOccupy(id,99,true);
		trains.getEdge(id)->electrified=false; trains.getEdge(id)->depotTrack=true;
		const int middle=roads.splitEdgeAt(id,350); trains.synchronize();
		context.expect(middle>=0 && trains.edges().size()==2,U"共通網の分割を鉄道の参照へ反映する");
		for (const auto& edge:trains.edges())
		{
			context.expect(!edge.electrified && edge.depotTrack,U"分割後も電化・車庫の属性を保持する");
			context.expect(edge.lanes.all([](const Lane& lane) { return lane.reservedBy<0; }),U"撤去された旧区間の予約を新しい区間へ複製しない");
		}
		const auto ids=roads.getNode(origin)->edgeIds();
		for (const int edge:ids) { roads.removeEdge(edge); }
		roads.removeNode(origin); trains.synchronize();
		context.expect(!trains.getNode(origin),U"撤去した共通ノードを駅として残さない");
		const JSON state=trains.saveState(); TrainNetwork restored; restored.bind(&roads);
		context.expect(restored.restoreState(state),U"線路撤去で経路が途切れても街全体の復元を失敗させない");
		context.expectEqual(restored.schedules().size(),size_t{1},U"経路不成立のダイヤを消さず修正用に保持する");
		TrainManager manager; manager.init(&restored); manager.update(.1,0);
		context.expect(manager.trains().isEmpty(),U"撤去された駅へは列車を出発させない");
	});
	runner.add(U"SharedTransport.GeneratedDoubleTrackLimits",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for(int z=0;z<7;++z) for(int x=0;x<7;++x) { world.installChunkDirect({x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20}); }
		MapGenerator::Settlement a,b; a.center={1000,1000};b.center={4100,3700};a.name=U"西町";b.name=U"東町";
		a.plan.station=b.plan.station=Vec2{0,0};b.gridAxisX={0,1};b.gridAxisZ={-1,0};
		RoadNetwork roads;TrainNetwork trains;RailwayAlignment::generate(trains,world,{a,b},&roads);
		context.expect(!trains.schedules().isEmpty(),U"駅の向きが違う街同士でも制約を守る鉄道を生成する");
		for(const auto& edge:trains.edges())
		{
			context.expect(&edge==roads.getEdge(edge.id),U"自動生成も共通網の路盤を使用する");
			if(edge.depotTrack) { continue; }
			context.expectEqual(edge.lanes.size(),size_t{2},U"本線は方向の異なる2軌道を持つ");
			context.expect(edge.lanes[0].dir!=edge.lanes[1].dir && !edge.lanes[0].bidirectional,U"上下線の方向を設定する");
			context.expect(RoadAlignment::respectsLimits(*trains.getBezier(edge.id),edge.roadType,TransportMode::Rail),U"完成した鉄道の曲率・勾配が制約以内");
		}
		const CubicBezier steep{{0,20,0},{0,25,100},{0,30,200},{0,35,300}};
		context.expect(!RoadAlignment::respectsLimits(steep,RoadType::LocalRoad,TransportMode::Rail),U"自動車向けの5%勾配は鉄道に流用しない");
	});
	runner.add(U"SharedTransport.EditorPreview",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,14}; font.preload(U"通常追越加速減速左折右折バス駐車待避禁止導流帯軌道舗装バラストスラブ");
		for(int i=0;i<3;++i) { System::Update(); }
		const RenderTexture target{Size{320,330},TextureFormat::R8G8B8A8_Unorm};
		{
			const ScopedRenderTarget2D scope{target.clear(ColorF{.12})};
			for(int i=0;i<12;++i)
			{
				Lane lane;lane.type=static_cast<LaneType>(i);
				TransportSectionControls::laneKind(font,lane,10,10+i*24,22);
				context.expect(font(TransportSectionControls::kLaneNames[i]).region().w<=60,U"全車線種別の日本語が既存の選択欄に収まる");
			}
			for(int i=0;i<3;++i)
			{
				RoadEdge edge;TransportCrossSection::railway(edge,true,i==1,i==2);
				TransportSectionControls::roadbed(font,edge.parts.front(),100,10+i*24,22);
			}
		}
		Graphics2D::Flush();Image image;target.readAsImage(image);int bright=0;
		for(const auto pixel:image) { bright+=pixel.r>150 && pixel.g>150 && pixel.b>150; }
		context.expect(bright>350,U"軌道・路盤切替UIをGPUで描画できる");
		image.save(U"Screenshot/shared_transport_controls.png");
		context.expectEqual(RoadPresetStore::buildDefaults().size(),size_t{7},U"従来の4道路と複線3断面を同じプリセット一覧で扱う");
	});
	runner.add(U"SharedTransport.CommonRoadbedGpuReview",[](TestContext& context)
	{
		RegisterAssets(); const FilePath directory = FileSystem::CurrentDirectory();
		struct Restore { FilePath path; ~Restore() { FileSystem::ChangeCurrentDirectory(path); } } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20});
		const Size size{640,480}; const BasicCamera3D camera{size,40_deg,Vec3{532,70,450},Vec3{512,20,530}};
		RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		RoadPartRegistry registry; registry.load(U"assets/road_parts");
		for (const String id : {U"roadbed_ballast",U"roadbed_slab"})
		{
			const auto& definition=registry.get(id);
			context.expect(definition.id==id && definition.texture && static_cast<bool>(*definition.texture),U"バラスト・スラブの実定義とテクスチャを読み込み、代替材質で済ませない");
		}
		JSON report;
		for(int variant=0;variant<3;++variant)
		{
			RoadNetwork roads;TrainNetwork trains;const int id=doubleTrack(roads,trains);
			TransportCrossSection::railway(*roads.getEdge(id),true,variant==1,variant==2);
			for(const auto& node:trains.nodes()) { trains.getNode(node.id)->type=TrackNodeType::Joint; }
			RoadRenderer roadRenderer;context.expect(roadRenderer.loadAssets(),U"共通路盤の実アセットを読み込む");
			roadRenderer.setCacheBuildBudget(Math::Inf);
			TrainRenderer trainRenderer;Array<Image> views;
			for(bool tracks:{false,true})
			{
				{
					const ScopedRenderTarget3D scope{target.clear(ColorF{.06})};
					const ScopedRenderStates3D depth{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.8});
					roadRenderer.render(roads,world,ViewFrustum{camera,24000},camera.getEyePosition());
					if(tracks) { trainRenderer.renderTracks(trains,world,camera.getEyePosition(),roads); }
				}
				Graphics3D::Flush();Image image;target.readAsImage(image);views<<std::move(image);
			}
			int bed=0,rail=0;double brightness=0;const Color background=views[0][0][0];
			for(int y=0;y<size.y;++y) for(int x=0;x<size.x;++x)
			{
				bed+=views[0][y][x]!=background;
				if (views[0][y][x]!=background) { const auto pixel=views[0][y][x]; brightness+=(pixel.r+pixel.g+pixel.b)/3.0; }
				rail+=views[0][y][x]!=views[1][y][x];
			}
			context.expect(bed>1000,U"道路レンダラーが鉄道・併用道路の路盤を描く");
			context.expect(rail>80,U"共通路盤の上に複線のレールが見える");
			roads.getEdge(id)->edgeState=EdgeState::Planned;
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{.06})};
				const ScopedRenderStates3D depth{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				trainRenderer.renderTracks(trains,world,camera.getEyePosition(),roads);
			}
			Graphics3D::Flush();Image planned;target.readAsImage(planned);int plannedPixels=0;
			for(const auto pixel:planned) { plannedPixels+=pixel!=planned[0][0]; }
			context.expectEqual(plannedPixels,0,U"未着工の計画路線に完成済みのレールを描かない");
			report[variant][U"bedPixels"]=bed;report[variant][U"railPixels"]=rail;
			report[variant][U"meanBedBrightness"]=brightness/Max(1,bed);
			views[1].save(directory+U"Screenshot/shared_transport_bed_{}.png"_fmt(variant));
		}
		context.expect(Abs(report[0][U"meanBedBrightness"].get<double>()-report[1][U"meanBedBrightness"].get<double>())>5,U"バラストとスラブの描画結果が異なる材質になる");
		report.save(directory+U"TestResults/shared_transport_gpu.json");
	});

	runner.add(U"SharedTransport.PlanningToolbarReview",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,14}, bold{FontMethod::MSDF,14,Typeface::Bold};
		font.preload(U"複線バラストスラブ軌道併設道路上下2車道40km/h始点終点経路生成調整確定着工");
		for(int i=0;i<3;++i) { System::Update(); }
		for(int preset=4;preset<=6;++preset)
		{
			const auto road=RoadPlanDraft::makeRoadTemplate(preset);
			context.expect(road.hasRailLanes() && road.lanes.size()==(preset==6 ? 4 : 2),U"道路計画の選択が実際の複線・併設断面を作る");
			const RenderTexture target{Size{370,440},TextureFormat::R8G8B8A8_Unorm};
			RoadPlanToolbar::State state;state.preset=preset;state.width=road.totalWidth();
			{
				const ScopedRenderTarget2D scope{target.clear(ColorF{.12})};
				RoadPlanToolbar::draw(font,bold,358,state);
			}
			Graphics2D::Flush();Image image;target.readAsImage(image);const Color background=image[439][369];int outside=0,changed=0;
			for(int y=0;y<440;++y) for(int x=0;x<370;++x)
			{
				if(image[y][x]==background) { continue; } ++changed;
				outside += x>=358 || y>RoadPlanToolbar::kHeight;
			}
			context.expect(changed>3000 && outside==0,U"鉄道の計画操作が既存パネルの大きさに収まる");
			image.save(U"Screenshot/rail_plan_{}.png"_fmt(preset));
		}
	});

}
