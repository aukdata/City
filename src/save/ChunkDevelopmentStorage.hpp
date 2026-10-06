#pragma once
#include <Siv3D.hpp>
#include "../world/Chunk.hpp"

/// @brief Persist the exact zoning and building layout without regenerating it on load.
namespace ChunkDevelopmentStorage
{
	/// @brief Write a validated chunk snapshot, then verify the completed file.
	bool write(const FilePath& path, const Chunk& chunk);
	/// @brief Restore only after every record is valid; leave the chunk unchanged on failure.
	bool read(const FilePath& path, Chunk& chunk);
}
