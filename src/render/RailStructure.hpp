#pragma once
#include "BridgeStructure.hpp"

/// @brief RC高架の床版・主桁・側壁を全て閉じた体積で作り、桁下からも欠けなく描く。
namespace RailStructure
{
	inline void prism(MeshData& mesh,Vec3 start,Vec3 end,Vec3 rightStart,Vec3 rightEnd,double offset,double halfWidth,double top,double bottom)
	{
		const Vec3 leftA=start+rightStart*(offset-halfWidth)+Vec3{0,top,0},rightA=start+rightStart*(offset+halfWidth)+Vec3{0,top,0};
		const Vec3 leftB=end+rightEnd*(offset-halfWidth)+Vec3{0,top,0},rightB=end+rightEnd*(offset+halfWidth)+Vec3{0,top,0};
		const Vec3 lowLeftA=leftA+Vec3{0,bottom-top,0},lowRightA=rightA+Vec3{0,bottom-top,0},lowLeftB=leftB+Vec3{0,bottom-top,0},lowRightB=rightB+Vec3{0,bottom-top,0};
		BridgeStructure::quad(mesh,leftA,leftB,rightB,rightA);
		BridgeStructure::quad(mesh,lowLeftA,lowRightA,lowRightB,lowLeftB);
		BridgeStructure::quad(mesh,leftA,lowLeftA,lowLeftB,leftB);
		BridgeStructure::quad(mesh,rightA,rightB,lowRightB,lowRightA);
		BridgeStructure::quad(mesh,leftA,rightA,lowRightA,lowLeftA);
		BridgeStructure::quad(mesh,leftB,lowLeftB,lowRightB,rightB);
	}
	inline MeshData deck(Vec3 start,Vec3 end,Vec3 rightStart,Vec3 rightEnd)
	{
		MeshData mesh;
		prism(mesh,start,end,rightStart,rightEnd,0,2.4,0,-.6);
		for (const int side : {-1,1})
		{
			prism(mesh,start,end,rightStart,rightEnd,side*1.25,.28,-.6,-1.5);
			prism(mesh,start,end,rightStart,rightEnd,side*2.3,.10,.75,0);
		}
		return mesh;
	}
}
