#include "GameScene.hpp"
#include "../road/RoadPlanConstruction.hpp"
#include "../ui/KeyboardActions.hpp"

namespace
{
	StringView roadPlanErrorMessage(RoadPlanConstruction::Error error)
	{
		switch (error)
		{
		case RoadPlanConstruction::Error::MissingRoute:
			return U"保存先の路線を選択してください";
		case RoadPlanConstruction::Error::InsufficientFunds:
			return U"着工資金が不足しています";
		case RoadPlanConstruction::Error::ConnectionFailed:
			return U"接続できません。重複や接続本数を確認";
		default:
			return U"";
		}
	}
}

void GameScene::clearDraftRoadPlan()
{
	m_draftRoadPlan.editor.clear();
	m_draftRoadPlan.draggedPoint = none;
	m_draftRoadPlan.dragPoints.clear();
	m_draftRoadPlan.message.clear();
	m_draftRoadPlan.error = false;
	m_roadPlanCursor = none;
}

bool GameScene::generateDraftRoadPlan()
{
	const Stopwatch timer{StartImmediately::Yes};
	const bool valid = m_draftRoadPlan.editor.generate(m_world,m_drawTemplate);
	m_draftRoadPlan.error = !valid;
	m_soundEffects.play(valid ? SoundEffects::Cue::Confirm : SoundEffects::Cue::Reject);
	m_draftRoadPlan.message = valid ? U"点や線をドラッグして調整できます" : U"経路が見つかりません。始点・終点を調整してください";
	DBG_LOG(U"[RoadPlan] generate valid={} points={} elapsedMs={:.3f}"_fmt(valid,m_draftRoadPlan.editor.points().size(),timer.msF()));
	return valid;
}

bool GameScene::rebuildDraftRoadPlan()
{
	const Stopwatch timer{StartImmediately::Yes};
	const bool valid = m_draftRoadPlan.editor.generated() && m_draftRoadPlan.editor.rebuild(m_world,m_drawTemplate);
	m_draftRoadPlan.error = !valid && m_draftRoadPlan.editor.generated();
	m_draftRoadPlan.message = m_draftRoadPlan.error ? U"勾配・接続条件を満たしません。点を調整してください" : U"";
	DBG_LOG(U"[RoadPlan] preview points={} valid={} length={:.1f} elapsedMs={:.3f}"_fmt(
		m_draftRoadPlan.editor.points().size(),valid,m_draftRoadPlan.editor.length(),timer.msF()));
	return valid;
}

bool GameScene::commitDraftRoadPlan()
{
	if (!m_draftRoadPlan.editor.valid() || !m_draftRoadPlan.editor.generated() || m_draftRoadPlan.draggedPoint) { return false; }
	if (m_draftRoadPlan.appendToExistingRoute && !m_draftRoadPlan.routeId)
	{
		m_soundEffects.play(SoundEffects::Cue::Reject);
		m_draftRoadPlan.message = roadPlanErrorMessage(RoadPlanConstruction::Error::MissingRoute);
		m_draftRoadPlan.error = true;
		return false;
	}
	const Stopwatch timer{ StartImmediately::Yes };
	const RoadPlanConstruction::Request request{
		m_draftRoadPlan.nameEdit.text, m_draftRoadPlan.routeNameEdit.text,
		m_draftRoadPlan.appendToExistingRoute ? m_draftRoadPlan.routeId : none, m_sandboxActive };
	const auto result = RoadPlanConstruction::commit(m_network, m_world, m_draftRoadPlan.editor,
		m_drawTemplate, request, m_economy.funds, m_clock.now);
	if (const auto* error = std::get_if<RoadPlanConstruction::Error>(&result))
	{
		if (*error == RoadPlanConstruction::Error::InvalidDraft) { return false; }
		m_soundEffects.play(SoundEffects::Cue::Reject);
		m_draftRoadPlan.message = roadPlanErrorMessage(*error);
		m_draftRoadPlan.error = true;
		DBG_LOG(U"[RoadPlan] commit failed reason={} elapsedMs={:.3f}"_fmt(static_cast<int>(*error), timer.msF()));
		return false;
	}

	const auto& receipt = std::get<RoadPlanConstruction::Receipt>(result);
	for (const int id : receipt.removedEdgeIds) { m_worldRenderer.invalidateTerrainForEdge(id); }
	applyConstructionStart(receipt.cost, receipt.edgeIds, receipt.affectedNodeIds);
	m_roadPlanSnapIndex.rebuild(m_network);
	selectRoadPlan(receipt.planId);
	clearDraftRoadPlan();
	m_draftRoadPlan.message = U"着工しました。工期が終わると通行できます";
	DBG_LOG(U"[RoadPlan] saved plan={} edges={} elapsedMs={:.3f}"_fmt(receipt.planId, receipt.edgeIds.size(), timer.msF()));
	return true;
}

void GameScene::handleRoadPlan()
{
	if (!m_panelManager.isVisible(U"draw_template")) { m_roadPlanCursor = none; return; }
	if (GameInput::down(KeyPageUp)) { m_drawElevation=Min(100.0f,m_drawElevation+1); }
	if (GameInput::down(KeyPageDown)) { m_drawElevation=Max(-100.0f,m_drawElevation-1); }
	if (GameInput::down(KeyEnter)) { commitDraftRoadPlan(); return; }
	const bool redo = GameInput::pressed(KeyControl) && (GameInput::down(KeyY) || (GameInput::pressed(KeyShift) && GameInput::down(KeyZ)));
	const bool undo = GameInput::down(KeyBackspace) || (GameInput::pressed(KeyControl) && !GameInput::pressed(KeyShift) && GameInput::down(KeyZ));
	if (redo || undo || (!m_panelManager.blocksMouseInput() && MouseR.down()))
	{
		m_draftRoadPlan.draggedPoint = none;
		m_draftRoadPlan.dragPoints.clear();
		if (redo ? m_draftRoadPlan.editor.redo() : m_draftRoadPlan.editor.undo()) { rebuildDraftRoadPlan(); }
		return;
	}
	m_roadPlanCursor = none;
	const auto& points = m_draftRoadPlan.editor.points();
	const bool onGround = !m_panelManager.blocksMouseInput() && m_cursorGroundPos.has_value();
	if (onGround)
	{
		Vec3 point = *m_cursorGroundPos;
		point.y += m_drawElevation;
		m_roadPlanCursor =
			m_draftRoadPlan.snapping && !GameInput::pressed(KeyAlt) && Abs(m_drawElevation) < .1f
				? m_roadPlanSnapIndex.find(m_network, point, 12, 6, !m_underground,
					  [&](const RoadEdge& edge) { return !m_underground || m_subsurface.containsEdge(edge.id); })
				: RoadPlanSnapIndex::Hit{point};
		if (m_underground && !SubsurfaceView::below(m_roadPlanCursor->position, m_world))
		{
			m_roadPlanCursor = RoadPlanSnapIndex::Hit{point};
		}
	}
	if (m_draftRoadPlan.draggedPoint)
	{
		if (onGround) { m_draftRoadPlan.dragPoints[*m_draftRoadPlan.draggedPoint] = m_roadPlanCursor->position; }
		if (!MouseL.pressed())
		{
			if (onGround && m_draftRoadPlan.editor.revise(std::move(m_draftRoadPlan.dragPoints))) { rebuildDraftRoadPlan(); }
			m_draftRoadPlan.draggedPoint = none;
			m_draftRoadPlan.dragPoints.clear();
		}
		return;
	}
	if (!onGround || !MouseL.down()) { return; }
	const auto screen = [&](Vec3 point) { return m_camera.camera3D().worldToScreenPoint(point+Vec3{0,1,0}).xy(); };
	constexpr double kHandleRadius = 12;
	Optional<size_t> selected;
	double nearest = kHandleRadius;
	for (size_t index = 0; index < points.size(); ++index)
	{
		const double distance = screen(points[index]).distanceFrom(Cursor::PosF());
		if (distance < nearest) { selected = index; nearest = distance; }
	}
	Array<Vec3> edited = points;
	if (!selected && m_draftRoadPlan.editor.generated())
	{
		// 曲線そのものを拾い、途中に新しい調整点を挿入する。
		size_t index = 0;
		for (const auto& edge : m_draftRoadPlan.editor.preview().edges())
		{
			if (edge.id < 0) { continue; }
			const auto curve = m_draftRoadPlan.editor.preview().getBezier(edge.id);
			++index;
			if (!curve) { continue; }
			for (int sample = 1; sample < 32; ++sample)
			{
				const Vec3 point = curve->evaluate(sample/32.0f);
				const double distance = screen(point).distanceFrom(Cursor::PosF());
				if (distance < nearest) { selected = index; nearest = distance; }
			}
		}
		if (selected) { edited.insert(edited.begin()+*selected,m_roadPlanCursor->position); }
	}
	if (selected)
	{
		m_draftRoadPlan.draggedPoint = selected;
		m_draftRoadPlan.dragPoints = std::move(edited);
	}
	else if (points.size() < 2)
	{
		if (!m_draftRoadPlan.editor.place(m_roadPlanCursor->position))
		{
			m_draftRoadPlan.error = true;
			m_draftRoadPlan.message = U"始点から2m以上離して指定してください";
		}
		else { m_soundEffects.play(SoundEffects::Cue::Select);m_draftRoadPlan.message.clear(); m_draftRoadPlan.error = false; }
	}
}

