#pragma once
#include <Siv3D.hpp>

/// @brief 開始時に選ぶ生成要素。依存関係と保存形式を全ての入口で共有する。
struct GenerationOptions
{
	enum class Element : uint8
	{
		Rivers,
		Settlements,
		Roads,
		Railway,
		Buildings,
		Farms,
		Trees,
		Cars,
		Pedestrians,
		Count
	};
	static constexpr size_t kCount = static_cast<size_t>(Element::Count);
	std::array<bool, kCount> selected{true, true, true, true, true, true, true, true, true};
	static constexpr std::array<StringView, kCount> labels{
		U"河川", U"町・村", U"道路", U"鉄道・駅", U"建物", U"田畑・畦道", U"自然の樹木", U"自動車", U"歩行者"};
	static constexpr std::array<StringView, kCount> keys{
		U"rivers", U"settlements", U"roads", U"railway", U"buildings", U"farms", U"trees", U"cars", U"pedestrians"};
	bool available(Element element) const
	{
		switch (element)
		{
		case Element::Roads:
			return enabled(Element::Settlements);
		case Element::Railway:
		case Element::Buildings:
			return enabled(Element::Roads);
		case Element::Farms:
		case Element::Cars:
		case Element::Pedestrians:
			return enabled(Element::Buildings);
		default:
			return true;
		}
	}
	bool enabled(Element element) const { return selected[static_cast<size_t>(element)] && available(element); }
	JSON save() const
	{
		JSON result;
		for (size_t i = 0; i < kCount; ++i)
		{
			result[keys[i]] = selected[i];
		}
		return result;
	}
	static GenerationOptions load(const JSON& value)
	{
		GenerationOptions result;
		for (size_t i = 0; i < kCount; ++i)
		{
			result.selected[i] = value[keys[i]].getOr<bool>(true);
		}
		return result;
	}
};
