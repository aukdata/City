#pragma once
#include "TestRunner.hpp"
#include "src/gen/StreetProfile.hpp"

/// @brief Test-only candidate selection on explicitly identified Village edges, with exact restoration.
namespace VillageGutterPreview
{
	class CandidateSelection
	{
		struct Saved
		{
			int edgeId;
			Array<RoadPart> parts;
		};
		RoadNetwork& m_roads;
		Array<Saved> m_saved;
		int m_changedParts = 0;
	public:
		/// @brief Verify the actual profile and replace only its two gutter definition IDs.
		/// @details Call after choosing the exact preview edges and before creating a fresh RoadRenderer.
		CandidateSelection(TestContext& context, RoadNetwork& roads, const Array<int>& villageEdgeIds) : m_roads(roads)
		{
			m_saved.reserve(villageEdgeIds.size());
			try
			{
			RoadEdge prototype;
			GeneratedStreet::apply(prototype, GeneratedStreet::describe(GeneratedStreet::Role::Village));
			HashSet<int> visited;
			for (const int edgeId : villageEdgeIds)
			{
				if (!visited.insert(edgeId).second) { continue; }
				auto* edge = roads.getEdge(edgeId);
				context.expect(edge != nullptr, U"Candidate preview targets a real explicitly selected Village edge");
				if (!edge) { continue; }
				bool exactProfile = edge->parts.size() == prototype.parts.size() && edge->lanes.size() == prototype.lanes.size();
				if (exactProfile)
				{
					for (size_t index = 0; index < prototype.parts.size(); ++index)
					{
						const auto& original = edge->parts[index];
						const auto& expected = prototype.parts[index];
						exactProfile &= original.type == expected.type && original.placement == expected.placement && original.envelopeRole == expected.envelopeRole
							&& Abs(original.offsetA_L - expected.offsetA_L) < 0.00001f && Abs(original.offsetA_R - expected.offsetA_R) < 0.00001f
							&& Abs(original.offsetB_L - expected.offsetB_L) < 0.00001f && Abs(original.offsetB_R - expected.offsetB_R) < 0.00001f;
						if (original.type == RoadPartType::RoadsideGutter)
						{
							exactProfile &= original.defId == U"roadside_gutter_concrete" || original.defId == U"roadside_gutter_covered_concrete";
						}
						exactProfile &= original.type != RoadPartType::Curb && original.type != RoadPartType::Sidewalk;
					}
				}
				context.expect(exactProfile, U"Preview selection matches a sidewalk-free Village section without widening or profile guessing");
				if (!exactProfile) { continue; }
				m_saved << Saved{edgeId, edge->parts};
				for (auto& part : edge->parts)
				{
					if (part.type != RoadPartType::RoadsideGutter) { continue; }
					part.defId = U"roadside_gutter_covered_concrete";
					++m_changedParts;
				}
			}
			}
			catch (...) { restore(); throw; }
		}
		CandidateSelection(const CandidateSelection&) = delete;
		CandidateSelection& operator=(const CandidateSelection&) = delete;
		~CandidateSelection() { restore(); }
		void restore()
		{
			for (auto& saved : m_saved)
			{
				if (auto* edge = m_roads.getEdge(saved.edgeId)) { edge->parts = std::move(saved.parts); }
			}
			m_saved.clear();
		}
		[[nodiscard]] int changedParts() const { return m_changedParts; }
	};
}
