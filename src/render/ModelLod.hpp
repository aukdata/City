#pragma once
#include <Siv3D.hpp>

#include "../asset/ModelLodPath.hpp"

/// @brief 近景・中景・遠景で同じ原点と材質を共有するモデル。
class ModelLod
{
public:
	explicit ModelLod(FilePath source = {}) : m_source{std::move(source)} {}
	Model& at(int level)
	{
		level = Clamp(level, 0, 2);
		if (!m_loaded[level])
		{
			m_loaded[level] = true;
			m_models[level] = Model{modelLodPath(m_source, level)};
			if (!m_models[level].isEmpty()) { Model::RegisterDiffuseTextures(m_models[level], TextureDesc::MippedSRGB); }
		}
		return m_models[level];
	}
	[[nodiscard]] const FilePath& source() const { return m_source; }
private:
	FilePath m_source;
	std::array<Model, 3> m_models;
	std::array<bool, 3> m_loaded{};
};
