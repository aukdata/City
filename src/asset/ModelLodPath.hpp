#pragma once
#include <Siv3D.hpp>

/// @brief 全OBJに共通の距離LODアセットパス。元モデルのディレクトリ構造を保持する。
inline FilePath modelLodPath(FilePathView source, int level)
{
	if (level <= 0) { return FilePath{source}; }
	const FilePath relative = FileSystem::RelativePath(FileSystem::FullPath(source), FileSystem::FullPath(U"assets/"));
	if (relative.isEmpty() || relative.starts_with(U"../") || relative.starts_with(U"/")
		|| relative.contains(U":") || !relative.ends_with(U".obj")) { return FilePath{source}; }
	return U"assets/lod/{}.lod{}.obj"_fmt(relative.substr(0, relative.size() - 4), Clamp(level, 1, 2));
}

