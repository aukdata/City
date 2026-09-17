#pragma once
#include "../road/RoadTypes.hpp"

/// @brief 道路断面と信号編集図で同じ部品・白線を同じ色で表すための表示規則。
namespace RoadDiagramStyle
{
	/// @brief RoadPartType → UI 描画色（断面バー / 信号編集図 共用）
	inline ColorF partTypeColor(RoadPartType type)
	{
		switch (type)
		{
		case RoadPartType::Roadbed:   return ColorF{0.25, 0.25, 0.27};
		case RoadPartType::Shoulder:  return ColorF{0.35, 0.33, 0.30};
		case RoadPartType::Median:    return ColorF{0.45, 0.55, 0.30};
		case RoadPartType::Sidewalk:  return ColorF{0.60, 0.58, 0.55};
		case RoadPartType::Gutter:    return ColorF{0.20, 0.20, 0.22};
		case RoadPartType::Guardrail: return ColorF{0.55, 0.55, 0.55};
		case RoadPartType::Wall:      return ColorF{0.45, 0.42, 0.38};
		case RoadPartType::Curb:      return ColorF{0.50, 0.48, 0.44};
		case RoadPartType::Slope:     return ColorF{0.40, 0.52, 0.30};
		case RoadPartType::BikeLane:  return ColorF{0.30, 0.45, 0.55};
		default:                      return ColorF{0.3};
		}
	}

	/// @brief LineType → 描画色（断面バー / 信号編集図 共用）
	/// @note DashedWhite は alpha=0.5（バー上で破線を視覚的に示す）
	inline ColorF lineTypeColor(LineType lt)
	{
		switch (lt)
		{
		case LineType::SolidWhite:  return ColorF{1.0, 1.0, 1.0};
		case LineType::DashedWhite: return ColorF{1.0, 1.0, 1.0, 0.5};
		case LineType::SolidYellow: return ColorF{1.0, 0.9, 0.0};
		case LineType::DoubleYellow:return ColorF{1.0, 0.9, 0.0};
		default:                    return ColorF{0, 0, 0, 0};
		}
	}
}
