// GuideSign/RoadSign テスト用スタブ: InferAutoForEdge/computeSignTransforms は呼ばれないため空実装
#include "../src/road/RoadNetwork.hpp"

RoadEdge*       RoadNetwork::getEdge(int)       { return nullptr; }
const RoadEdge* RoadNetwork::getEdge(int) const { return nullptr; }
RoadNode*       RoadNetwork::getNode(int)       { return nullptr; }
const RoadNode* RoadNetwork::getNode(int) const { return nullptr; }
Optional<CubicBezier> RoadNetwork::getBezier(int) const { return none; }
const String* RoadNetwork::getDestinationName(int) const { return nullptr; }
const String* RoadNetwork::getDestinationReading(int) const { return nullptr; }
uint8 RoadNetwork::getDestinationTier(int) const { return 0; }
RoadRoute*       RoadNetwork::getRoute(int)       { return nullptr; }
const RoadRoute* RoadNetwork::getRoute(int) const { return nullptr; }
