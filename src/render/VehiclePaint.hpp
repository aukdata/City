#pragma once
#include "../traffic/Vehicle.hpp"

/// @brief 個体IDから選ぶ車体色。距離・再描画・シミュレーション更新で色を変えない。
namespace VehiclePaint
{
	inline ColorF color(const Vehicle& vehicle)
	{
		if (vehicle.type!=VehicleType::PassengerCar && vehicle.type!=VehicleType::KeiCar) { return ColorF{.8,.8,.78}; }
		static const Array<ColorF> palette=[]
		{
			const auto document=JSON::Load(U"assets/vehicles/paint.json");
			if (!document || !document[U"linearColors"].isArray()) { throw Error{U"車体色の設定を読み込めません"}; }
			Array<ColorF> result;
			for (const auto& item:document[U"linearColors"].arrayView())
			{
				Array<double> values;for (const auto& component:item.arrayView()) { values << component.get<double>(); }
				if (values.size()!=3 || std::any_of(values.begin(),values.end(),[](double x){return !std::isfinite(x) || x<0 || x>1;})) { throw Error{U"車体色には0〜1のRGBが必要です"}; }
				result << ColorF{values[0],values[1],values[2]};
			}
			if (result.isEmpty()) { throw Error{U"車体色を1色以上指定してください"}; }
			return result;
		}();
		uint32 value=static_cast<uint32>(vehicle.id);value^=value>>16;value*=0x7feb352du;value^=value>>15;
		return palette[value%palette.size()];
	}
}
