#include "GameScene.hpp"
#include "../ui/PanelWidget.hpp"

/// @brief Opt-in diagnostics for real keyboard/mouse playtests; never transmits images.
void GameScene::recordPlaytestFrame()
{
	if (m_playtestFrame == 0)
	{
		m_playtestFrames.open(U"playtest_frames.csv");
		m_playtestFrames.writeln(U"frame,mode,map,frameMs,renderMs,logicMs,terrainMs,roadMs,shadowMs,cars,carUpdateMs,carDrawMs,carDrawCalls,visibleCars,buildingDrawCalls,gpuMs,renderDistanceMeters,buildingsSubmitted,treesSubmitted");
	}
	m_playtestFrames.writeln(U"{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.3f},{:.3f},{},{},{},{:.3f},{},{},{}"_fmt(
		m_playtestFrame,static_cast<int>(m_camera.mode()),m_minimapRenderer.fullScreen() ? 1 : 0,
		Scene::DeltaTime()*1000,m_renderTimings.total,m_logicMs,m_renderTimings.terrain,m_renderTimings.roadMesh,m_cityLighting.shadowMilliseconds(),
		m_vehicleManager.vehicleCount(),m_vehicleManager.lastStats().total(),m_renderTimings.vehicle,m_vehicleRenderer.drawCalls(),
		m_vehicleRenderer.submitted(),m_worldRenderer.buildingDrawCalls(),m_gpuTimer.milliseconds(),
		getData().renderDistance,m_worldRenderer.buildingsSubmitted(),m_worldRenderer.treesSubmitted()));
	if(m_renderTimings.total>40)
	{
		const auto& cache=m_roadRenderer.cacheBuildStats();
		DBG_LOG(U"[TravelCost] frame={} total={:.2f} terrain={:.2f} road={:.2f} parts={:.2f} node={:.2f} marks={:.2f} furniture={:.2f} fallback={:.2f} edges={} nodes={}"_fmt(m_playtestFrame,m_renderTimings.total,m_renderTimings.terrainOnly,m_renderTimings.roadMesh,cache.partsMs,cache.nodesMs,cache.markingsMs,cache.furnitureMs,cache.fallbackMs,cache.edges,cache.nodes));
	}
	bool inputChanged = false;
	for (int code=0;code<256;++code)
	{
		const Input key{InputDeviceType::Keyboard,static_cast<uint8>(code)};
		if (GameInput::down(key)) { DBG_LOG(U"[PlaytestInput] frame={} key={}"_fmt(m_playtestFrame,key.name())); inputChanged=true; }
	}
	if (MouseL.down() || MouseR.down() || Mouse::Wheel()!=0) { inputChanged=true; }
	if (inputChanged || m_playtestFrame%15==0)
	{
		JSON state;
		const Vec3 focus=m_camera.focusPoint(); auto& map=m_minimapRenderer.mapView();
		state[U"commandId"]=m_playtestCommandId;
		state[U"fpsGraph"]=m_frameRateGraph.visible;state[U"fps"]=m_frameRateGraph.latest();
		state[U"selectionKind"]=static_cast<int>(m_selection.kind);state[U"selectionId"]=m_selection.id;
		if (const auto selected = selectedEdgeId())
		{
			if (const auto* edge = m_network.getEdge(*selected))
			{
				state[U"selectedEdgeSpeedLimit"] = edge->speedLimit;
			}
			if (const auto* edge = m_simGraph ? m_simGraph->getEdge(*selected) : nullptr)
			{
				state[U"selectedEdgeSimulationSpeedLimit"] = edge->speedLimit;
			}
		}
		state[U"townLabels"] = Array<JSON>{};
		int labelIndex = 0;
		for (const auto& label : m_placeNameRenderer.labels())
		{
			JSON item; item[U"name"] = label.name;
			item[U"anchor"] = Array<double>{label.anchor.x, label.anchor.y};
			item[U"bounds"] = Array<double>{label.bounds.x, label.bounds.y, label.bounds.w, label.bounds.h};
			state[U"townLabels"][labelIndex++] = item;
		}
		const auto& residents=m_pedestrianManager.stats();
		state[U"pedestrians"][U"population"]=residents.population;
		state[U"pedestrians"][U"walking"]=residents.walking;
		state[U"pedestrians"][U"waitingTrain"]=residents.waitingTrain;
		state[U"pedestrians"][U"ridingTrain"]=residents.ridingTrain;
		state[U"pedestrians"][U"waitingCar"]=residents.waitingCar;
		state[U"pedestrians"][U"ridingCar"]=residents.ridingCar;
		state[U"pedestrians"][U"completed"]=residents.completed;
		state[U"pedestrians"][U"updateMs"]=residents.updateMs;
		state[U"pedestrians"][U"renderMs"]=m_renderTimings.pedestrian;
		state[U"pedestrians"][U"drawCalls"]=m_pedestrianRenderer.stats().drawCalls;
		state[U"pedestrians"][U"visible"]=m_pedestrianRenderer.stats().submitted;
		state[U"railway"][U"editorVisible"]=m_panelManager.isVisible(U"rail_timetable");
		state[U"railway"][U"message"]=m_trainTimetableEditor.message;
		state[U"railway"][U"edgeCount"]=m_trainNetwork.edges().size();
		state[U"railway"][U"depots"]=m_trainNetwork.depots().size();
		state[U"railway"][U"trains"]=Array<JSON>{};
		int trainIndex=0;
		for (const auto& train : m_trainManager.trains())
		{
			JSON item; item[U"id"]=train.id; item[U"line"]=train.scheduleId; item[U"speed"]=train.speed; item[U"passengers"]=train.passengerCount;
			item[U"state"]=static_cast<int>(train.state); item[U"stopIndex"]=train.nextStopIdx;
			item[U"position"]=Array<double>{train.position.x,train.position.y,train.position.z};
			state[U"railway"][U"trains"][trainIndex++]=item;
		}
		int stationIndex=0;
		for (const auto& node : m_trainNetwork.nodes())
		{
			if (node.type!=TrackNodeType::Station) { continue; }
			JSON item; item[U"id"]=node.id; item[U"name"]=node.name; item[U"position"]=Array<double>{node.position.x,node.position.y,node.position.z};
			state[U"railway"][U"stations"][stationIndex++]=item;
		}
		int lineIndex=0;
		for (const auto& line : m_trainNetwork.schedules())
		{
			JSON item; item[U"id"]=line.id; item[U"name"]=line.name; item[U"enabled"]=line.enabled;
			item[U"first"]=line.firstDepartureMinute; item[U"last"]=line.lastDepartureMinute; item[U"interval"]=line.headwaySec;
			item[U"next"]=RailTimetable::nextDeparture(line,m_clock.now).value_or(-1);
			item[U"stops"]=Array<int>{};
			for (size_t stop=0;stop<line.stops.size();++stop) { item[U"stops"][stop]=line.stops[stop].stationNodeId; }
			state[U"railway"][U"lines"][lineIndex++]=item;
		}
		state[U"zone"]=static_cast<int>(m_paintZone);
		state[U"development"]=m_zoneManager.developmentSnapshot();
		state[U"zonePalette"]=m_panelManager.isVisible(U"zone_palette");
		state[U"commandPalette"]=m_commandPalette.visible;
		state[U"commandInput"]=m_commandPalette.input;
		state[U"commandMessage"]=m_commandPalette.message;
		state[U"renderDistanceMeters"]=getData().renderDistance;
		state[U"renderQuality"]=getData().lowSpec ? U"light" : U"standard";
		state[U"settingsVisible"]=m_settings.visible;
		state[U"settingsError"]=m_settings.error;
		state[U"effectVolume"]=getData().effectVolume;
		state[U"renderTargetWidth"]=getData().lowSpec ? m_lowSpecRenderTexture.width() : m_renderTexture.width();
		state[U"renderTargetHeight"]=getData().lowSpec ? m_lowSpecRenderTexture.height() : m_renderTexture.height();
		state[U"buildingsSubmitted"]=m_worldRenderer.buildingsSubmitted();
		state[U"treesSubmitted"]=m_worldRenderer.treesSubmitted();
		state[U"zoneBrush"]=m_zoneBrushRadius;
		int zoneIndex=0;
		for (int gy=static_cast<int>(Floor(focus.z/16))-8;gy<=static_cast<int>(Floor(focus.z/16))+8;++gy)
		{
			for (int gx=static_cast<int>(Floor(focus.x/16))-8;gx<=static_cast<int>(Floor(focus.x/16))+8;++gx)
			{
				if (gx<0 || gy<0 || gx>=WORLD_CHUNKS*ZONE_CELLS || gy>=WORLD_CHUNKS*ZONE_CELLS) { continue; }
				const Point coord{gx/ZONE_CELLS,gy/ZONE_CELLS},cell{gx%ZONE_CELLS,gy%ZONE_CELLS};
				const auto* chunk=m_world.getChunk(coord);if (!chunk) { continue; }
				const auto& building=chunk->buildingGrid[cell];
				JSON item;item[U"x"]=(gx+.5)*16;item[U"z"]=(gy+.5)*16;
				item[U"zone"]=static_cast<int>(chunk->zoneMap[cell]);item[U"building"]=static_cast<int>(building.type);
				item[U"builtAt"]=building.builtAt;item[U"edge"]=building.edgeId;
				item[U"offsetX"]=building.offsetX;item[U"offsetZ"]=building.offsetZ;item[U"angle"]=building.angle;
				state[U"zoneCells"][zoneIndex++]=item;
			}
		}

		state[U"driving"]=m_driving.active();
		state[U"sound"][U"volume"]=m_soundEffects.volume();state[U"sound"][U"enginePlaying"]=m_soundEffects.drivingPlaying();
		state[U"sound"][U"events"]=m_soundEffects.playedCount();state[U"sound"][U"enginePitch"]=m_soundEffects.mix().pitch;
		double audioPeak=0;
		for (const float sample : GlobalAudio::BusGetSamples(MixBus1)) { audioPeak=Max(audioPeak,Abs(static_cast<double>(sample))); }
		state[U"sound"][U"outputPeak"]=audioPeak;
		if (m_driving.active())
		{
			const auto& car=m_driving.vehicle();
			state[U"car"][U"position"]=Array<double>{car.position.x,car.position.y,car.position.z};
			state[U"car"][U"heading"]=car.heading;state[U"car"][U"pitch"]=car.pitch;
			state[U"car"][U"speed"]=car.speed;state[U"car"][U"distance"]=m_driving.distance();
			state[U"car"][U"blocked"]=m_driving.blocked();state[U"car"][U"edge"]=car.currentEdge;
		}
		int nearbyIndex=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0 || nearbyIndex>=20) { continue; }
			const auto curve=m_network.getBezier(edge.id);if (!curve) { continue; }
			const Vec3 middle=curve->positionAt(curve->totalLength*.5f);
			if (Vec2{middle.x-focus.x,middle.z-focus.z}.length()>400) { continue; }
			JSON item;
			item[U"id"]=edge.id;item[U"length"]=edge.length;item[U"width"]=edge.totalWidth();item[U"open"]=edge.edgeState==EdgeState::Open || edge.edgeState==EdgeState::Existing;
			for (int i=0;i<=12;++i)
			{
				const Vec3 point=curve->positionAt(curve->totalLength*i/12);
				item[U"points"][i]=Array<double>{point.x,point.y,point.z};
			}
			state[U"nearbyRoads"][nearbyIndex++]=item;
		}
		if (const auto selected=selectedRoadPlanId())
		{
			if (const auto* plan=m_network.getPlan(*selected))
			{
				state[U"selectedPlan"][U"id"]=plan->id;state[U"selectedPlan"][U"length"]=plan->totalLength;
				state[U"selectedPlan"][U"seconds"]=plan->constructionDuration;state[U"selectedPlan"][U"state"]=static_cast<int>(plan->state);
			}
		}
		state[U"draftValid"]=m_draftRoadPlan.editor.valid();state[U"draftMessage"]=m_draftRoadPlan.message;
		state[U"frame"]=m_playtestFrame; state[U"focused"]=Window::GetState().focused;
		state[U"cameraMode"]=static_cast<int>(m_camera.mode()); state[U"editMode"]=static_cast<int>(m_mode);
		state[U"focusX"]=focus.x; state[U"focusY"]=focus.y; state[U"focusZ"]=focus.z;
		state[U"eyeY"]=m_camera.eyePosition().y; state[U"pauseMenu"]=m_showPauseMenu;
		state[U"textInput"]=PanelWidget::activeTextInput!=nullptr;
		state[U"map"]=map.visible; state[U"mapX"]=map.center.x; state[U"mapZ"]=map.center.y; state[U"mapZoom"]=map.zoom;
		state[U"mapContext"]=map.contextWorld.has_value();
		state[U"mapCustomPixelShader"]=Graphics2D::GetCustomPixelShader().has_value();
		state[U"localMapCenter"]=Array<double>{m_minimapRenderer.localView().center.x,m_minimapRenderer.localView().center.y};
		state[U"localMapSpan"]=m_minimapRenderer.localView().span(); state[U"localMapHeadingUp"]=m_minimapRenderer.localView().headingUp;
		state[U"draftPoints"]=m_draftRoadPlan.editor.points().size(); state[U"plans"]=m_network.plans().size();
		state[U"funds"]=m_economy.funds; state[U"population"]=m_economy.population;
		state[U"income"]=m_economy.monthlyGrant(); state[U"maintenance"]=m_economy.roadMaintenanceCost(m_network);
		state[U"gameHour"]=m_clock.hour; state[U"timeSpeed"]=static_cast<int>(m_clock.speed);
		state[U"traffic"][U"cars"]=m_vehicleManager.vehicleCount();
		state[U"traffic"][U"accessPoints"]=m_vehicleManager.buildingAccess().size();
		state[U"traffic"][U"spawned"]=m_vehicleManager.populationStats().spawned;
		state[U"traffic"][U"completed"]=m_vehicleManager.populationStats().completed;
		state[U"traffic"][U"routeFailures"]=m_vehicleManager.populationStats().routeFailures;
		state[U"traffic"][U"localCars"]=m_vehicleManager.populationStats().localVehicles;
		state[U"traffic"][U"localTarget"]=m_vehicleManager.populationStats().localTarget;
		state[U"traffic"][U"updateMs"]=m_vehicleManager.lastStats().total();
		int stopped=0; for (const auto& car : m_vehicleManager.vehicles()) { stopped += car.speed < .5f; }
		state[U"traffic"][U"stopped"]=stopped;
		state[U"renderMs"]=m_renderTimings.total; state[U"logicMs"]=m_logicMs;
		state[U"signalsMs"]=m_renderTimings.signals; state[U"mapBuilds"]=map.cartographyBuilds();
		state[U"cursorX"]=Cursor::Pos().x; state[U"cursorY"]=Cursor::Pos().y;
		state.save(U"playtest_state.json");
		if (inputChanged) { DBG_LOG(U"[PlaytestState] {}"_fmt(state.formatMinimum())); }
	}
	if ((GameInput::down(KeyF10) || GameInput::down(KeyF12))) { ScreenCapture::SaveCurrentFrame(U"playtest_{:06}.png"_fmt(m_playtestFrame)); }
	++m_playtestFrame;
}

/// @brief Exercise cold navigation changes through the same camera methods as actual play.
void GameScene::updateNavigationBenchmark()
{
	constexpr int kPhaseFrames = 180;
	constexpr int kPhases = 6;
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13;
	m_camera.setBlockInput(true);
	if (!m_benchmarkOrigin)
	{
		m_benchmarkOrigin = m_camera.focusPoint();
		m_playtestFrames.open(U"navigation_frames.csv");
		m_playtestFrames.writeln(U"phase,frame,cpuMs,roadsMs,terrainMs,shadowMs,edges,nodes,markings,furniture,partsMs,nodesMs,markingsMs,furnitureMs,deferred,fallbackMs");
	}
	const Vec3 origin = *m_benchmarkOrigin;
	if (m_captureFrame == 0)
	{
		if (m_captureIndex == 1 || m_captureIndex == 2) { m_camera.cycleMode(); }
		if (m_captureIndex == 3)
		{
			Vec3 destination = origin + Vec3{-1800, 0, -900};
			destination.y = m_world.sampleHeight(static_cast<float>(destination.x), static_cast<float>(destination.z));
			m_camera.setFocus(destination);
			m_camera.cycleMode();
		}
		if (m_captureIndex == 4) { m_camera.setWalkingState(m_camera.focusPoint(), static_cast<float>(Math::Pi)); }
		if (m_captureIndex == 5) { m_camera.setFocus(origin); m_camera.cycleMode(); }
		DBG_LOG(U"[Navigation] phase={} focus={} eye={}"_fmt(m_captureIndex, m_camera.focusPoint(), m_camera.eyePosition()));
	}
	const Stopwatch timer{StartImmediately::Yes};
	m_world.update(m_camera.focusPoint());
	renderWorld();
	const double cpuMs = timer.msF();
	const auto& stats = m_roadRenderer.cacheBuildStats();
	m_playtestFrames.writeln(U"{},{},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.3f}"_fmt(
		m_captureIndex, m_captureFrame, cpuMs, m_renderTimings.roadMesh, m_renderTimings.terrainOnly,
		m_cityLighting.shadowMilliseconds(), stats.edges, stats.nodes, stats.markings, stats.furniture,
		stats.partsMs, stats.nodesMs, stats.markingsMs, stats.furnitureMs, stats.deferred, stats.fallbackMs));
	if (m_captureFrame == 0 || cpuMs > 40)
	{
		DBG_LOG(U"[NavigationCost] phase={} frame={} cpuMs={:.2f} roadsMs={:.2f} edges={} nodes={} partsMs={:.2f} nodesMs={:.2f} markingsMs={:.2f} furnitureMs={:.2f}"_fmt(
			m_captureIndex, m_captureFrame, cpuMs, m_renderTimings.roadMesh, stats.edges, stats.nodes,
			stats.partsMs, stats.nodesMs, stats.markingsMs, stats.furnitureMs));
	}
	if (m_captureFrame == 60 && (m_worldRenderer.pendingTerrainJobs() > 0 || stats.deferred > 0)) { return; }
	if (m_captureFrame == kPhaseFrames - 1) { ScreenCapture::SaveCurrentFrame(U"navigation_{}.png"_fmt(m_captureIndex)); }
	if (++m_captureFrame == kPhaseFrames)
	{
		m_captureFrame = 0;
		if (++m_captureIndex == kPhases)
		{
			m_playtestFrames.close();
			DBG_LOG(U"[Navigation] complete phases={}"_fmt(kPhases));
			System::Exit();
		}
	}
}

/// @brief Explicit local playtest control; unused unless a command file is supplied on launch.
void GameScene::processPlaytestCommand()
{
	const JSON command=JSON::Load(getData().playtestCommands);
	if (!command) { return; }
	if (!command.contains(U"id") || !command.contains(U"action")) { return; }
	const int id=command[U"id"].getOr<int>(0);
	if (id<=m_playtestCommandId) { return; }
	m_playtestCommandId=id;
	const String action=command[U"action"].getOr<String>(U"");
	const auto number=[&](StringView key,double fallback=0.0) { return command.contains(key) ? command[key].getOr<double>(fallback) : fallback; };
	if (action==U"drive")
	{
		m_playtestDrivingInput={Clamp(number(U"throttle"),0.0,1.0),Clamp(number(U"reverse"),0.0,1.0),Clamp(number(U"steering"),-1.0,1.0),command.contains(U"brake") && command[U"brake"].getOr<bool>(false)};
		m_playtestDrivingUntil=Scene::Time()+Clamp(number(U"seconds",1),0.0,15.0);
	}
	else if (action==U"view")
	{
		jumpToMapPosition({number(U"x",m_camera.focusPoint().x),number(U"z",m_camera.focusPoint().z)});
	}
	else if (action==U"benchmark_view")
	{
		const double x=number(U"x",m_camera.focusPoint().x),z=number(U"z",m_camera.focusPoint().z);
		m_camera.setCaptureState({x,number(U"y",m_world.sampleHeight(static_cast<float>(x),static_cast<float>(z))),z},
			static_cast<float>(number(U"distance",3000)),static_cast<float>(number(U"yaw",-.7)),static_cast<float>(number(U"pitch",.72)));
		if (command.contains(U"hour")) { m_clock.now += (number(U"hour")-m_clock.hour)*GameClock::kSecondsPerGameHour; m_clock.syncCalendar(); }
		m_clock.speed=static_cast<TimeSpeed>(Clamp(static_cast<int>(number(U"speed",0)),0,3));
	}
	else if (action==U"key")
	{
		const int code=static_cast<int>(number(U"code"));
		if (InRange(code,1,255))
		{
			const auto original=GameInput::buffer;
			GameInput::buffer=KeyboardActionBuffer{};
			GameInput::buffer.update({{0,0,static_cast<uint8>(code),true,false}},true);
			handleGlobalShortcuts();
			handleInput();
			GameInput::buffer=original;
		}
	}
	else if (action==U"plan" && command.contains(U"points"))
	{
		leaveDriving(true);clearDraftRoadPlan();m_mode=EditMode::RoadPlan;
		m_draftRoadPlan.preset=Clamp(static_cast<int>(number(U"preset")),0,3);
		m_drawTemplate=RoadPlanDraft::makeRoadTemplate(m_draftRoadPlan.preset);
		for (const auto& point : command[U"points"].arrayView())
		{
			const double x=point[0].get<double>(),z=point[1].get<double>();
			m_draftRoadPlan.editor.place({x,m_world.sampleHeight(static_cast<float>(x),static_cast<float>(z)),z});
		}
		m_roadPlanSnapIndex.rebuild(m_network);
		m_panelManager.show(U"draw_template",U"道路計画  [R]",panelRightPos(U"draw_template"));
	}
	else if (action==U"zone")
	{
		setZonePaintMode(true);
		m_paintZone=static_cast<ZoneType>(Clamp(static_cast<int>(number(U"zone",2)),0,6));
		m_zoneBrushRadius=Clamp(static_cast<int>(number(U"radius",0)),0,2);
		m_zoneManager.paintZone(m_world,{number(U"x",m_camera.focusPoint().x),0,number(U"z",m_camera.focusPoint().z)},m_paintZone,m_zoneBrushRadius);
	}
	else if (action==U"rail_select")
	{
		setRailTimetableVisible(true);
		if (const auto* line=m_trainNetwork.getSchedule(static_cast<int>(number(U"line")))) { m_trainTimetableEditor.select(*line); }
	}
	else if (action==U"rail_edit")
	{
		if (command.contains(U"name")) { m_trainTimetableEditor.name.text=command[U"name"].get<String>(); }
		if (command.contains(U"first")) { m_trainTimetableEditor.first.text=command[U"first"].get<String>(); }
		if (command.contains(U"last")) { m_trainTimetableEditor.last.text=command[U"last"].get<String>(); }
		if (command.contains(U"interval")) { m_trainTimetableEditor.interval.text=command[U"interval"].get<String>(); }
		if (command.contains(U"enabled")) { m_trainTimetableEditor.enabled=command[U"enabled"].get<bool>(); }
	}
	else if (action==U"rail_apply") { m_trainTimetableEditor.apply(m_trainNetwork); }
	else if (action==U"rail_depot") { addRailDepot(static_cast<int>(number(U"station"))); }
	else if (action==U"rail_view")
	{
		if (const auto* node=m_trainNetwork.getNode(static_cast<int>(number(U"station")))) { m_camera.setCaptureState(node->position,static_cast<float>(number(U"distance",240)),-.7f,.72f); }
	}
	else if (action==U"rail_save_review") { getData().saveName=U"railway_review"; saveGame(); }
	else if (action==U"volume") { getData().effectVolume=Clamp(number(U"value",.6),0.0,1.0); }
	else if (action==U"command") { executeCommand(command[U"text"].getOr<String>(U"")); }
	else if (action==U"palette_text" && m_commandPalette.visible) { m_commandPalette.input=command[U"text"].getOr<String>(U"/"); }
	else if (action==U"generate") { generateDraftRoadPlan(); }
	else if (action==U"commit") { commitDraftRoadPlan(); }
	else if (action==U"select_parcel") { selectLandParcelAt({number(U"x"),number(U"z")}); }
	else if (action == U"select_train")
	{
		selectTrain(static_cast<int>(number(U"train")));
	}
	else if (action==U"map_open") { m_minimapRenderer.openFullScreen(m_camera,m_network,m_trainNetwork,m_districts); }
	else if (action==U"map_pan") { m_minimapRenderer.mapView().pan({number(U"x"),number(U"y")},Scene::Size()); }
	else if (action==U"map_zoom") { m_minimapRenderer.mapView().zoomAt(m_minimapRenderer.mapView().body(Scene::Size()).center(),number(U"factor",1),Scene::Size()); }
	else if (action==U"map_close") { m_minimapRenderer.mapView().close(); }
	else if (action==U"local_map") { auto& view=m_minimapRenderer.localView();view.changeZoom(static_cast<int>(number(U"zoom")));if (number(U"toggle")!=0) { view.headingUp=!view.headingUp; } }
	else if (action==U"pointer") { Cursor::SetPos(Point{static_cast<int>(number(U"x")),static_cast<int>(number(U"y"))}); }
	else if (action==U"audit_signals")
	{
		JSON report;int minor=0,major=0,signals=0;report[U"minorSignalNodes"]=Array<int>{};
		for(const auto& node:m_network.nodes())
		{
			if(node.id<0 || node.attachments.size()<3) { continue; }
			bool narrow=true;
			for(const auto& attachment:node.attachments)
			{
				const auto* edge=m_network.getEdge(attachment.edgeId);if(!edge) { continue; }
				narrow &= edge->farmAccess || (edge->roadType==RoadType::LocalRoad && edge->lanes.size()<=2);
			}
			(narrow ? minor : major)++;signals+=node.signalPlacement.has_value();
			if(narrow && node.signalPlacement) { report[U"minorSignalNodes"].push_back(node.id); }
		}
		report[U"minorJunctions"]=minor;report[U"majorJunctions"]=major;report[U"signalJunctions"]=signals;report.save(U"playtest_signals.json");
	}
	else if (action==U"snapshot") { ScreenCapture::SaveCurrentFrame(U"playtest_command_{}.png"_fmt(id)); }
	else if (action==U"exit") { System::Exit(); }
	if (number(U"capture")!=0) { ScreenCapture::SaveCurrentFrame(U"playtest_command_{}.png"_fmt(id)); }
	DBG_LOG(U"[PlaytestCommand] id={} action={}"_fmt(id,action));
}

/// @brief 城下の外縁と田畑への出入りを、生成した位置で撮影・数値記録する。
void GameScene::prepareFringeCapture(int variant)
{
	const MapGenerator::Settlement* town=nullptr;
	for (const auto& candidate : m_districts)
	{
		if (candidate.plan.origin==UrbanMorphology::Origin::Castle && !candidate.plan.fringeStreets.isEmpty()
			&& (!town || candidate.plan.halfExtent.x>town->plan.halfExtent.x)) { town=&candidate; }
	}
	if (!town) { return; }
	const auto global=[&](Vec2 point) { return town->center+town->gridAxisX*point.x+town->gridAxisZ*point.y; };
	const auto position=[&](Vec2 point)
	{
		return Vec3{point.x,m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)),point.y};
	};
	if (variant==0)
	{
		m_camera.setCaptureState(position(town->center),static_cast<float>(Min(1800.0,town->plan.halfExtent.x*3.6+600)), -.7f,1.28f);
		JSON report; report[U"halfExtent"]=Array<double>{town->plan.halfExtent.x,town->plan.halfExtent.y};
		int roadIndex=0,houseIndex=0,fringeHouses=0;
		const auto local=[&](Vec2 point)
		{
			const Vec2 delta=point-town->center;
			return Vec2{delta.dot(town->gridAxisX),delta.dot(town->gridAxisZ)};
		};
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0) { continue; }
			const auto curve=m_network.getBezier(edge.id);
			if (!curve) { continue; }
			const Vec3 middle=curve->positionAt(curve->totalLength*.5f);
			if (Vec2{middle.x,middle.z}.distanceFrom(town->center)>2200) { continue; }
			JSON points; const int count=Max(1,static_cast<int>(Ceil(curve->totalLength/20)));
			for (int sample=0;sample<=count;++sample)
			{
				const Vec3 p=curve->positionAt(curve->totalLength*sample/count); const Vec2 point=local({p.x,p.z});
				points[sample]=Array<double>{point.x,point.y};
			}
			report[U"roads"][roadIndex++]=points;
		}
		for (int z=0;z<WORLD_CHUNKS;++z)
		{
			for (int x=0;x<WORLD_CHUNKS;++x)
			{
				const auto* chunk=m_world.getChunk(Point{x,z});
				if (!chunk || Vec2{(x+.5)*CHUNK_SIZE,(z+.5)*CHUNK_SIZE}.distanceFrom(town->center)>3000) { continue; }
				for (int row=0;row<ZONE_CELLS;++row)
				{
					for (int col=0;col<ZONE_CELLS;++col)
					{
						const auto& house=chunk->buildingGrid[{col,row}];
						if (house.type==BuildingType::None || house.type==BuildingType::Farmland) { continue; }
						const Vec2 point=local({x*CHUNK_SIZE+(col+.5)*16+house.offsetX,z*CHUNK_SIZE+(row+.5)*16+house.offsetZ});
						if (point.length()>2200) { continue; }
						report[U"houses"][houseIndex++]=Array<double>{point.x,point.y};
						fringeHouses+=!UrbanMorphology::inCore(town->plan,point,22) && UrbanMorphology::contains(town->plan,point);
					}
				}
			}
		}
		report[U"fringeHouses"]=fringeHouses;
		report.save(U"Screenshot/"+getData().captureFolder+U"/castle_layout.json");
		DBG_LOG(U"[FringeCapture] roads={} houses={} fringeHouses={} extent={}"_fmt(roadIndex,houseIndex,fringeHouses,town->plan.halfExtent));
		return;
	}
	if (variant==1)
	{
		const Line street=town->plan.fringeStreets.back();
		m_camera.setCaptureState(position(global((street.begin+street.end)*.5)),380,-.7f,.85f);
		return;
	}
	const LandPatch* field=nullptr;
	double nearest=Math::Inf;
	for (int z=0;z<WORLD_CHUNKS;++z)
	{
		for (int x=0;x<WORLD_CHUNKS;++x)
		{
			const auto* chunk=m_world.getChunk(Point{x,z}); if (!chunk) { continue; }
			for (const auto& patch : chunk->landPatches)
			{
				if (patch.type!=LandPatchType::PaddyField || patch.polygon.size()!=4) { continue; }
				const double distance=patch.polygon.front().distanceFrom(town->center);
				if (distance<nearest) { nearest=distance; field=&patch; }
			}
		}
	}
	if (!field) { return; }
	const Vec2 middle=(field->polygon[0]+field->polygon[2])*.5;
	if (variant==2) { m_camera.setCaptureState(position(middle),370,-.7f,1.0f); }
	else { m_camera.setWalkingState(position({field->polygon[0].x-4.8,middle.y}),0.0f); }
	DBG_LOG(U"[FarmCapture] variant={} field={} eye={}"_fmt(variant,middle,m_camera.eyePosition()));
}

/// @brief 新規生成された実店舗を用途別に撮影し、配置数・敷地寸法を記録する。
void GameScene::prepareRoadsideCapture(int variant)
{
	const BuildingType requested = static_cast<BuildingType>(static_cast<int>(BuildingType::UrbanConvenience)+variant);
	bool focused = false;
	Array<int> counts(5, 0);
	JSON report;
	for (int z = 0; z < WORLD_CHUNKS; ++z)
	{
		for (int x = 0; x < WORLD_CHUNKS; ++x)
		{
			const auto* chunk = m_world.getChunk({x,z});
			if (!chunk) { continue; }
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const auto& building = chunk->buildingGrid[{col,row}];
					if (!isRoadsideServiceBuilding(building.type) && building.type!=BuildingType::RuralHouse) { continue; }
					const int category = static_cast<int>(building.type)-static_cast<int>(BuildingType::UrbanConvenience);
					++counts[category];
					const Vec3 point{x*CHUNK_SIZE+(col+.5)*16+building.offsetX,0,z*CHUNK_SIZE+(row+.5)*16+building.offsetZ};
					if (building.type != requested || focused) { continue; }
					const double ground = m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
					m_camera.setCaptureState({point.x,ground+2,point.z},buildingFootprintXZ(building.type)*2.3f+22,
						-building.angle-.5f,.68f);
					report[U"position"] = Array<double>{point.x,ground,point.z};
					report[U"siteWidth"] = buildingFootprintXZ(building.type);
					focused = true;
				}
			}
		}
	}
	report[U"counts"] = counts; report[U"found"] = focused;
	report.save(U"Screenshot/"+getData().captureFolder+U"/roadside_{}.json"_fmt(variant));
	DBG_LOG(U"[RoadsideCapture] variant={} found={} urbanShop={} ruralShop={} urbanFuel={} ruralFuel={} ruralHomes={}"_fmt(
		variant,focused,counts[0],counts[1],counts[2],counts[3],counts[4]));
}
