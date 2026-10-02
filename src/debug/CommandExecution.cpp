#include "CommandExecution.hpp"
#include "../render/RenderDistance.hpp"

CommandExecution::Result CommandExecution::execute(const GameCommands::Command& command,Context context)
{
	using Kind=GameCommands::Kind;
	const auto& n=command.numbers;Result result;result.success=true;
	switch(command.kind)
	{
	case Kind::Help:
		result.message=U"/time /day /road /camera /money /speed /fps /render  ·  説明書: plan/28_commands.md";break;
	case Kind::Time:
	case Kind::Day:
	{
		const double minute=GameClock::calendarMinuteFromTime(context.clock.now);
		double target=0;
		if(command.kind==Kind::Time) { target=Floor(minute/1440)*1440+n[0]*60+n[1]; }
		else { target=((n[0]-1)*360+(n[1]-1)*30+n[2]-1)*1440+Fmod(minute,1440); }
		context.clock.now=(target-GameClock::kInitialCalendarMinute)/GameClock::kCalendarMinutesPerSecond;
		context.clock.syncCalendar();result.changedTime=true;result.message=context.clock.timeString();break;
	}
	case Kind::RoadStatus:
	case Kind::RoadInspect:
	{
		const int id=static_cast<int>(n[0]);auto* edge=context.roads.getEdge(id);
		if(!edge) { return {false,U"道路ID {} は存在しません"_fmt(id),{},false}; }
		if(command.kind==Kind::RoadStatus)
		{
			// 一区間だけを変更するデバッグ操作。元の計画が後でこの区間を再開通させないよう離脱する。
			if(auto* plan=context.roads.getPlan(edge->planId))
			{
				const int planId=plan->id;plan->edgeIds.remove(id);edge->planId=-1;
				if(plan->edgeIds.isEmpty()) { context.roads.removePlan(planId); } else { context.roads.rebuildPlanStats(planId); }
			}
			edge->edgeState=command.argument==U"planned" ? EdgeState::Planned : command.argument==U"closed" ? EdgeState::Closed
				: command.argument==U"construction" ? EdgeState::UnderConstruction : EdgeState::Open;
			const bool built=edge->edgeState==EdgeState::Open || edge->edgeState==EdgeState::Closed;
			for(auto& part:edge->parts) { part.build=built ? BuildState::Built : edge->edgeState==EdgeState::UnderConstruction ? BuildState::UnderConstruction : BuildState::NotBuilt; }
			for(auto& lane:edge->lanes) { lane.op=edge->edgeState==EdgeState::Open ? OpState::Open : OpState::Closed; }
			edge->constructionStartTime=context.clock.now;
			context.roads.rebuildNodeConnectivity(edge->nodeA,edge->nodeB);
			result.changedEdges << id;
		}
		const String status=edge->edgeState==EdgeState::Planned ? U"planned" : edge->edgeState==EdgeState::UnderConstruction ? U"construction"
			: edge->edgeState==EdgeState::Closed ? U"closed" : U"open";
		result.message=U"道路 {}: {} / {:.0f} m / {}"_fmt(id,status,edge->length,edge->tunnel ? U"トンネル" : edge->useElevation ? U"高架" : U"地上");break;
	}
	case Kind::CameraGoto:
	{
		context.camera.setOverviewState({n[0],context.world.sampleHeight(static_cast<float>(n[0]),static_cast<float>(n[1])),n[1]},context.camera.distance(),context.camera.yaw(),context.camera.pitch());
		result.message=U"座標 ({:.0f}, {:.0f}) へ移動"_fmt(n[0],n[1]);break;
	}
	case Kind::CameraZoom:
		context.camera.setOverviewState(context.camera.focusPoint(),static_cast<float>(n[0]),context.camera.yaw(),context.camera.pitch());result.message=U"俯瞰距離 {:.0f} m"_fmt(n[0]);break;
	case Kind::Money: context.funds=n[0];result.message=U"資金 {:.2f} 億円"_fmt(n[0]);break;
	case Kind::Speed:
		context.clock.speed=n[0]==0 ? TimeSpeed::Paused : n[0]==1 ? TimeSpeed::x1 : n[0]==2 ? TimeSpeed::x2 : TimeSpeed::x4;
		result.message=U"時間速度 {}"_fmt(context.clock.speedString());break;
	case Kind::RenderDistance:
		if (!n.isEmpty())
		{
			if (!RenderDistance::valid(n[0])) { return {false,U"描画距離は 0 または 100～20000 m です",{},false}; }
			context.renderDistance=n[0];
		}
		result.message=(context.renderDistance==RenderDistance::kDefault
			? U"描画距離: 既定（追加制限なし）" : U"描画距離: {} m"_fmt(context.renderDistance))
			+ U" · /render distance 100～20000 · 0:既定へ戻す";
		break;
	case Kind::Fps: context.fps=command.argument==U"on";result.message=context.fps ? U"FPSグラフを表示" : U"FPSグラフを非表示";break;
	}
	return result;
}
