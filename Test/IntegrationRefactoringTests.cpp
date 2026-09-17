#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/RoadNodeIndex.hpp"
#include "src/road/RoadGeometry.hpp"

namespace
{
	/// @brief バケット検索とは独立した全件走査を基準に、取りこぼしと距離の丸めを確認する。
	Optional<int> nearestByScan(const RoadNetwork& network, Vec3 point, float radius)
	{
		float bestDistance = radius * radius;
		Optional<int> best;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0) { continue; }
			const float dx = static_cast<float>(node.position.x) - static_cast<float>(point.x);
			const float dz = static_cast<float>(node.position.z) - static_cast<float>(point.z);
			const float distance = dx * dx + dz * dz;
			if (distance < bestDistance) { bestDistance = distance; best = node.id; }
		}
		return best;
	}
}

void registerIntegrationRefactoringTests(TestRunner& runner)
{
	runner.add(U"IntegrationRefactoring.NodeIndexMatchesFullScan", [](TestContext& context)
	{
		RoadNetwork roads;
		RoadNodeIndex index;
		for (int i = 0; i < 113; ++i)
		{
			const Vec3 position{(i * 173 % 2011) - 1005.25, i * 4.0, (i * 419 % 2017) - 1008.75};
			index.insert(position, roads.addNode(position));
		}
		// 削除済み ID とバケットの負座標を含め、空間索引の都合で候補が変わらないことを確かめる。
		roads.removeNode(0);
		for (int i = 0; i < 73; ++i)
		{
			const Vec3 point{(i * 199 % 1709) - 854.13, -900, (i * 337 % 1721) - 860.37};
			for (const float radius : {1.0f, 50.0f, 199.0f, 200.0f, 201.0f, 650.0f})
			{
				context.expect(index.findNearest(point, roads, radius) == nearestByScan(roads, point, radius),
					U"負座標・バケット境界・削除済みノードを含む検索が全件走査と一致する");
			}
		}
	});

	runner.add(U"IntegrationRefactoring.NodeIndexSelectionPolicy", [](TestContext& context)
	{
		RoadNetwork roads;
		RoadNodeIndex index;
		const int isolated = roads.addNode({1, 0, 0});
		const int first = roads.addNode({10, 100, 0});
		const int second = roads.addNode({20, 200, 0});
		roads.addEdge(first, second, {13, 130, 0}, {17, 170, 0});
		for (const auto& node : roads.nodes()) { index.insert(node.position, node.id); }
		context.expect(index.findNearest({0, 0, 0}, roads, 30) == isolated, U"街路の吸着は未接続のノードも選ぶ");
		context.expect(!index.findNearest({0, 0, 0}, roads, 1), U"半径ちょうどの点は従来どおり含めない");
		context.expect(!index.findNearest({0, 0, 0}, roads, 0), U"空の検索半径は未発見を返す");
		const auto access = index.findBest({0, 0, 0}, roads, 30, [=](const RoadNode& node, float) -> Optional<double>
		{
			if (node.attachments.isEmpty()) { return none; }
			return node.id == second ? 1.0 : 2.0;
		});
		context.expect(access == second, U"集落の接続は距離だけでなく呼出側の費用と適格条件で決められる");
		const auto tied = index.findBest({0, 0, 0}, roads, 30, [](const RoadNode&, float) -> Optional<double> { return 1.0; });
		context.expect(tied == isolated, U"同点では既存の登録・走査順を維持する");
		index.clear();
		index.insert(roads.getNode(second)->position, second);
		context.expect(index.findNearest({0, 0, 0}, roads, 30) == second, U"索引再構築後は新しい段階の候補だけを使う");
	});

	runner.add(U"IntegrationRefactoring.SignalAnchorAsymmetricRoad", [](TestContext& context)
	{
		RoadNetwork roads;
		const int a = roads.addNode({100, 20, 100});
		const int b = roads.addNode({100, 20, 300});
		const int id = *roads.addEdge(a, b, {100, 20, 160}, {100, 20, 240});
		auto& edge = *roads.getEdge(id);
		edge.cutoffA = 12; edge.cutoffB = 18;
		edge.parts.clear();
		RoadPart roadbed;
		roadbed.type = RoadPartType::Roadbed;
		roadbed.offsetA_L = -3; roadbed.offsetA_R = 4;
		roadbed.offsetB_L = -5; roadbed.offsetB_R = 6;
		edge.parts << roadbed;
		RoadPart shoulder = roadbed;
		shoulder.type = RoadPartType::Sidewalk;
		shoulder.offsetA_L = -20; shoulder.offsetB_R = 20;
		edge.parts << shoulder;
		const auto curve = roads.getBezier(id);
		const auto fromA = RoadGeometry::signalAnchor(edge, *curve, a);
		const auto fromB = RoadGeometry::signalAnchor(edge, *curve, b);
		context.expect(fromA && fromB, U"道路の両端に信号を配置できる");
		if (!fromA || !fromB) { return; }
		context.expectNear(fromA->position.x, 103, 1e-6, U"A端の車道左端を使い、歩道端には移動しない");
		context.expectNear(fromB->position.x, 94, 1e-6, U"B端では反対側の非対称断面を使う");
		context.expectNear(fromA->position.z, 112, .2, U"A端の交差点切り欠きから配置する");
		context.expectNear(fromB->position.z, 282, .2, U"B端の切り欠きを終端から測る");
		context.expectNear(fromA->yaw, 0, 1e-6, U"A端は進入車両に向く");
		context.expectNear(Math::Abs(fromB->yaw), Math::Pi, 1e-6, U"B端は反対方向を向く");
		context.expect(!RoadGeometry::signalAnchor(edge, *curve, b + 100), U"未接続ノードには配置しない");
	});

	runner.add(U"IntegrationRefactoring.SignalAnchorCurveAndFallback", [](TestContext& context)
	{
		RoadNetwork roads;
		const int a = roads.addNode({50, 30, 50});
		const int b = roads.addNode({250, 60, 250});
		const int id = *roads.addEdge(a, b, {50, 40, 160}, {140, 50, 250});
		auto& edge = *roads.getEdge(id);
		edge.parts.clear(); edge.cutoffA = 9; edge.cutoffB = 14;
		const auto curve = roads.getBezier(id);
		for (const int node : {a, b})
		{
			const auto anchor = RoadGeometry::signalAnchor(edge, *curve, node);
			context.expect(anchor.has_value(), U"部品未展開の道路も幅から配置できる");
			if (!anchor) { continue; }
			const Vec3 lateral = anchor->position - anchor->roadPosition;
			const float arc = node == a ? edge.cutoffA : curve->totalLength - edge.cutoffB;
			const Vec3 tangent = curve->tangentAt(arc);
			context.expectNear(lateral.length(), edge.totalWidth() * .5, 1e-5, U"車道部品がない場合は全幅の半分を使う");
			context.expectNear(lateral.dot(tangent), 0, 1e-5, U"曲線の接線に直交する路肩へ配置する");
			context.expectNear(anchor->position.y, curve->positionAt(arc).y, 1e-6, U"設計高度を保持し、地上・高架の高度選択は描画側に任せる");
		}
	});
}
