#pragma once
#include "SaveResult.hpp"

/// @brief 保存ルート直下の完成済みセーブだけを一覧・削除の対象にする。
namespace SaveCatalog
{
	Array<String> list(FilePathView root);
	SaveResult remove(FilePathView root, StringView name);
} // namespace SaveCatalog
