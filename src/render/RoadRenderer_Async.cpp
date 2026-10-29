#include "RoadRenderer.hpp"
#include "MeshUploadSlices.hpp"
#include "RoadRenderDetail.hpp"
#include "../debug/DebugLog.hpp"
#include <algorithm>
#include <thread>

/// @file
/// @brief Workers own immutable equipment inputs. The render thread uploads and publishes complete results.

void RoadRenderer::startDetailJob(const RoadEdge& edge, const RoadNetwork& network, const World& world,
	float marginA, float marginB)
{
	if (hasPublishedDetails(edge.id)) { return; }
	const bool running = std::any_of(m_detailJobs.begin(), m_detailJobs.end(),
		[&](const DetailJob& job) { return job.edge == edge.id; });
	if (running) { return; }
	if (m_detailJobs.size() >= 2 || !canBuildCache()) { ++m_cacheBuildStats.deferred; return; }

	const Stopwatch timer{StartImmediately::Yes};
	const auto curve = network.getBezier(edge.id);
	if (!curve) { return; }
	Vec2 lower{Math::Inf, Math::Inf}, upper{-Math::Inf, -Math::Inf};
	for (const auto point : {curve->p0, curve->p1, curve->p2, curve->p3})
	{
		lower.x = Min(lower.x, point.x); lower.y = Min(lower.y, point.z);
		upper.x = Max(upper.x, point.x); upper.y = Max(upper.y, point.z);
	}
	Array<GuideSignPlacement> guides;
	double padding = RoadRenderDetail::equipmentPadding(edge);
	for (const auto& guide : network.guideSigns())
	{
		if (guide.id < 0 || guide.parentEdgeId != edge.id || guide.elements.isEmpty()) { continue; }
		guides << guide;
		padding = Max(padding, Abs(static_cast<double>(guide.lateralOffset)) + 32);
	}
	DetailInput input{edge, *curve, HeightSnapshot{world, RectF{lower, upper - lower}.stretched(padding)},
		LocationSigns::boundaries(*curve, m_municipalityLookup), std::move(guides),
		RoadSign::poleMetadata(), GuideSign::poleMetadata(), marginA, marginB};
	for (auto& part : input.edge.parts)
	{
		if (part.defId.isEmpty()) { continue; }
		const auto& definition = m_partRegistry.get(part.defId);
		if (part.useDefinitionRepeatSpacing) { part.repeatSpacing = definition.repeatSpacing; }
		if (part.useDefinitionRepeatJitter) { part.repeatJitter = definition.repeatJitter; }
	}
	DetailJob job{edge.id, m_detailEpoch, m_detailRevisions[edge.id], {}};
	job.future = std::async(std::launch::async, [input = std::move(input)]
	{
		const Stopwatch timer{StartImmediately::Yes};
		DetailResult result;
		for (auto& data : buildFurnitureMeshData(input.edge, input.curve, input.heights, input.marginA, input.marginB))
		{
			for (auto& slice : MeshUploadSlices::split(std::move(data.mesh)))
			{
				result.furniture << LaneLineMeshData{data.color, std::move(slice), data.cable};
			}
		}
		result.signs = buildSnapshotSigns(input);
		result.guides = buildSnapshotGuides(input);
		result.milliseconds = timer.msF();
		return result;
	});
	m_detailJobs << std::move(job);
	m_cacheBuildStats.furnitureMs += timer.msF();
}

void RoadRenderer::uploadCompletedDetails()
{
	const Stopwatch timer{StartImmediately::Yes};
	constexpr double kUploadBudgetMs = 1.0;
	for (size_t index = 0; index < m_detailJobs.size();)
	{
		auto& job = m_detailJobs[index];
		if (!job.result)
		{
			if (job.future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) { ++index; continue; }
			job.result = job.future.get();
		}
		if (job.epoch != m_detailEpoch || job.revision != m_detailRevisions[job.edge])
		{
			DBG_LOG(U"[RoadDetail] discard edge={} revision={} reason=staleSnapshot"_fmt(job.edge, job.revision));
			m_detailJobs.erase(m_detailJobs.begin() + index);
			continue;
		}
		auto& result = *job.result;
		while (job.furnitureIndex < result.furniture.size() && timer.msF() < kUploadBudgetMs)
		{
			auto& data = result.furniture[job.furnitureIndex++];
			job.uploaded << LaneLineBatch{data.color, Mesh{data.mesh}, data.cable};
			data.mesh = {};
		}
		while (job.furnitureIndex == result.furniture.size() && job.guideIndex < result.guides.size()
			&& timer.msF() < kUploadBudgetMs)
		{
			auto& data = result.guides[job.guideIndex++];
			GuideSignDraw draw;
			draw.poleMat = data.poleMat; draw.boardMat = data.boardMat;
			draw.poleTop = data.poleTop; draw.texKey = data.texKey;
			draw.boardMesh = Mesh{data.board}; draw.backMesh = Mesh{data.back};
			job.uploadedGuides << std::move(draw);
			data.board = {}; data.back = {};
		}
		if (job.furnitureIndex == result.furniture.size() && job.guideIndex == result.guides.size())
		{
			m_streetFurnitureCache[job.edge] = std::move(job.uploaded);
			m_signCache[job.edge] = std::move(result.signs);
			m_guideSignCache[job.edge] = std::move(job.uploadedGuides);
			++m_geometryRevision; ++m_cacheBuildStats.furniture;
			DBG_LOG(U"[RoadDetail] publish edge={} revision={} workerMs={:.2f} uploadMs={:.2f}"_fmt(
				job.edge, job.revision, result.milliseconds, timer.msF()));
			m_detailJobs.erase(m_detailJobs.begin() + index);
		}
		break;
	}
	m_cacheBuildStats.furnitureMs += timer.msF();
}

void RoadRenderer::preloadDetails(const RoadNetwork& network, const World& world, Vec3 eye)
{
	if (!m_asyncDetails) { return; }
	struct RestoreBudget
	{
		double& budget;
		double previous;
		~RestoreBudget() { budget = previous; }
	} restore{m_cacheBuildBudgetMs, m_cacheBuildBudgetMs};
	m_cacheBuildBudgetMs = Math::Inf;
	const Stopwatch timer{StartImmediately::Yes};
	Array<int> edges;
	for (const auto& edge : network.edges())
	{
		if (edge.id < 0 || !edge.isRoadbedBuilt() ||
			(edge.edgeState != EdgeState::Open && edge.edgeState != EdgeState::Existing)) { continue; }
		const auto bounds = edgeBounds(network, edge.id);
		if (Vec3{bounds.center}.distanceFrom(eye) > kPrepareDist + Sqrt(bounds.radiusSq)) { continue; }
		edges << edge.id;
	}
	size_t next = 0;
	while (next < edges.size() || !m_detailJobs.isEmpty())
	{
		uploadCompletedDetails();
		while (next < edges.size() && m_detailJobs.size() < 2)
		{
			const auto* edge = network.getEdge(edges[next++]);
			if (edge) { startDetailJob(*edge, network, world, edgeMargin(*edge, edge->nodeA), edgeMargin(*edge, edge->nodeB)); }
		}
		if (!m_detailJobs.isEmpty()) { std::this_thread::sleep_for(std::chrono::milliseconds{1}); }
	}
	m_cacheBuildStats = {};
	DBG_LOG(U"[RoadDetail] preload edges={} ms={:.2f}"_fmt(edges.size(), timer.msF()));
}
