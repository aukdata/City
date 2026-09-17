#pragma once
#include <Siv3D.hpp>

/// @brief 同期生成と非同期アップロードで共用する景観材質の色。
namespace LandscapeMaterials
{
	inline ColorF detailColorForKey(int key)
	{
		key%=1000;
		switch (key)
		{
		case 100: return ColorF{ 0.44, 0.44, 0.40 };
		case 101: return ColorF{ 0.38, 0.46, 0.29 };
		case 102: return ColorF{ 0.62, 0.60, 0.54 };
		case 103: return ColorF{ 0.42, 0.39, 0.34 };
		case 104: return ColorF{ 0.56, 0.63, 0.42 };
		case 105: return ColorF{ 0.32, 0.24, 0.18 };
		case 125: return ColorF{ 0.30, 0.40, 0.22 }.removeSRGBCurve();
		case 126: return ColorF{ 0.34, 0.44, 0.24 }.removeSRGBCurve();
		case 127: return ColorF{ .24,.20,.105 };
		case 130: case 131: case 134: return ColorF{1};
		case 132: return ColorF{.22,.34,.31}.removeSRGBCurve();
		case 133: return ColorF{.39,.23,.17}.removeSRGBCurve();
		case 135: return ColorF{.13,.21,.24}.removeSRGBCurve();
		case 128: return ColorF{.30,.40,.22}.removeSRGBCurve();
		case 129: return ColorF{.34,.44,.24}.removeSRGBCurve();
		case 124:
		case 106: return ColorF{ 0.18, 0.23, 0.11 };
		case 107: return ColorF{ 0.72, 0.74, 0.73 };
		case 108: return ColorF{ 0.35, 0.39, 0.43 };
		case 109: return ColorF{ 0.23, 0.24, 0.23 };
		case 110: return ColorF{ 0.31, 0.39, 0.31 };
		case 111: return ColorF{ 0.76, 0.70, 0.52 };
		case 112: return ColorF{ 0.68, 0.20, 0.16 };
		case 113: return ColorF{ 0.47, 0.44, 0.28 };
		case 114: return ColorF{ 0.66, 0.58, 0.36 };
		case 115: return ColorF{ 0.22, 0.21, 0.19 };
		case 116: return ColorF{ 0.16, 0.23, 0.25 };
		case 117: return ColorF{ 0.54, 0.54, 0.50 };
		case 118: return ColorF{ 0.32, 0.30, 0.23 };
		case 119: return ColorF{ 0.38, 0.42, 0.44 };
		case 120: return ColorF{ 0.25, 0.24, 0.20 };
		case 121: return ColorF{ 0.34, 0.31, 0.22 };
		case 122: return ColorF{ 0.26, 0.30, 0.18 };
		case 123: return ColorF{ 0.28, 0.29, 0.26 };
		default: return ColorF{ 0.60, 0.60, 0.60 };
		}
	}

}
