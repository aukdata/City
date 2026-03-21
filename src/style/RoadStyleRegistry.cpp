# include "../../stdafx.h"
#include "RoadStyleRegistry.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 内部ユーティリティ
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief TOML 配列から ColorF を読む（配列長 < 3 またはキーなしはデフォルト値）
	ColorF parseColorArray(const TOMLValue& v, ColorF def)
	{
		if (v.isEmpty() || v.arrayCount() < 3) return def;
		const auto arr = v.arrayView();
		return ColorF{
			arr[0].getOr<double>(def.r),
			arr[1].getOr<double>(def.g),
			arr[2].getOr<double>(def.b)
		};
	}

	/// @brief TOML サブテーブルから LineMarkStyle を読む
	LineMarkStyle parseLineMarkStyle(const TOMLValue& sec, const LineMarkStyle& def)
	{
		if (sec.isEmpty()) return def;
		LineMarkStyle s;
		s.color      = parseColorArray(sec[U"color"], def.color);
		s.lineWidth  = static_cast<float>(sec[U"line_width"].getOr<double>(def.lineWidth));
		s.dashLength = static_cast<float>(sec[U"dash_length"].getOr<double>(def.dashLength));
		s.gapLength  = static_cast<float>(sec[U"gap_length"].getOr<double>(def.gapLength));
		return s;
	}

	/// @brief TOML セクションから RoadStyle を構築する
	RoadStyle parseRoadStyle(const TOMLValue& sec)
	{
		RoadStyle style;

		// ---- 路面 ----
		style.surface.surfaceColor = parseColorArray(
			sec[U"surface_color"], style.surface.surfaceColor);
		style.surface.surfaceTileV = static_cast<float>(
			sec[U"surface_tile_v"].getOr<double>(style.surface.surfaceTileV));
		style.surface.surfaceTexturePath = sec[U"surface_texture"].getOr<String>(U"");

		if (!style.surface.surfaceTexturePath.isEmpty())
		{
			Texture tex{ style.surface.surfaceTexturePath, TextureDesc::Mipped };
			if (!tex.isEmpty())
				style.surface.surfaceTexture = std::move(tex);
		}

		// ---- 断面寸法 ----
		style.defaultLaneWidth = static_cast<float>(
			sec[U"default_lane_width"].getOr<double>(style.defaultLaneWidth));
		style.shoulderWidth    = static_cast<float>(
			sec[U"shoulder_width"].getOr<double>(style.shoulderWidth));
		style.medianWidth      = static_cast<float>(
			sec[U"median_width"].getOr<double>(style.medianWidth));

		// ---- 車線区画線 ----
		LineMarkStyle defLaneMark;
		defLaneMark.color      = ColorF{ 1.0, 1.0, 1.0 };
		defLaneMark.lineWidth  = 0.15f;
		defLaneMark.dashLength = 8.0f;
		defLaneMark.gapLength  = 12.0f;
		style.laneMarking = parseLineMarkStyle(sec[U"lane_marking"], defLaneMark);

		// ---- センターライン ----
		LineMarkStyle defCenter;
		defCenter.color      = ColorF{ 1.0, 1.0, 0.0 };
		defCenter.lineWidth  = 0.20f;
		defCenter.dashLength = 0.0f;
		defCenter.gapLength  = 0.0f;
		style.centerLine = parseLineMarkStyle(sec[U"center_line"], defCenter);

		return style;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// RoadStyleRegistry 実装
// ─────────────────────────────────────────────────────────────────────────────

bool RoadStyleRegistry::load(FilePathView dirPath)
{
	// ファイル名ステム（lower_snake_case）と RoadType の対応表
	const HashTable<String, RoadType> kStemToType = {
		{ U"local_road",  RoadType::LocalRoad  },
		{ U"arterial",    RoadType::Arterial   },
		{ U"expressway",  RoadType::Expressway },
		{ U"highway",     RoadType::Highway    },
	};

	bool anyLoaded = false;
	for (const FilePath& path : FileSystem::DirectoryContents(dirPath, Recursive::No))
	{
		if (FileSystem::Extension(path) != U"toml") continue;

		const String stem = FileSystem::BaseName(path);
		const auto   it   = kStemToType.find(stem);
		if (it == kStemToType.end()) continue;

		const TOMLReader reader{ path };
		if (!reader) continue;

		m_styles[static_cast<uint8>(it->second)] = parseRoadStyle(reader);
		anyLoaded = true;
	}

	return anyLoaded;
}

const RoadStyle& RoadStyleRegistry::get(RoadType rt) const
{
	const auto it = m_styles.find(static_cast<uint8>(rt));
	return (it != m_styles.end()) ? it->second : m_fallback;
}
