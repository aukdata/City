#pragma once
#include "../world/World.hpp"

/// @brief カメラモード
enum class CameraMode
{
	Overview,     ///< 俯瞰（デフォルト）
	Follow,       ///< 車両追従（後方視点）
	FirstPerson,  ///< 一人称（車両内視点）
};

/// @brief ゲームカメラ（BasicCamera3D ラッパー）
/// @details WASD 移動・ホイールクリックドラッグ回転・ホイールズームを提供する
class GameCamera
{
public:
	GameCamera();

	/// @brief 毎フレーム呼ぶ（入力処理・行列更新）
	/// @param dt    リアル経過秒
	/// @param world 地形クエリに使用（地形床クランプ・軌道ピボット計算）
	void update(double dt, const World& world);

	/// @brief 車両追従・一人称モード用のターゲットを設定する
	/// @param pos     車両ワールド座標
	/// @param heading 進行方向 [rad]（Y軸回り）
	void setFollowTarget(Vec3 pos, float heading);

	const BasicCamera3D& camera3D() const { return m_camera; }

	Vec3  focusPoint() const { return m_focus; }
	float distance()   const { return m_distance; }
	float yaw()        const { return m_yaw; }
	float pitch()      const { return m_pitch; }
	CameraMode mode()  const { return m_mode; }

	/// @brief カメラの視点位置を返す
	Vec3 eyePosition() const
	{
		return m_focus + Vec3{
			Math::Sin(m_yaw)  * Math::Cos(m_pitch),
			Math::Sin(m_pitch),
			Math::Cos(m_yaw)  * Math::Cos(m_pitch)
		} * m_distance;
	}

	/// @brief 注視点を設定する（初期化用。地形床クランプは適用しない）
	void setFocus(Vec3 focus);

	/// @brief スクリーン座標からグラウンド（y=0）上のワールド座標を返す
	/// @return 地面と交差しない場合は none
	Optional<Vec3> screenToGround(Vec2 screenPos) const;

	/// @brief スクリーン座標からレイを返す
	Ray screenToRay(Vec2 screenPos) const;

	/// @brief カメラモードを次に切り替える（Overview → Follow → FirstPerson → Overview）
	void cycleMode() { m_mode = static_cast<CameraMode>((static_cast<int>(m_mode) + 1) % 3); }

private:
	BasicCamera3D m_camera;
	CameraMode    m_mode     = CameraMode::Overview;

	Vec3  m_focus    = Vec3{ 512.0, 0.0, 512.0 };   ///< 注視点
	float m_yaw      = 0.0f;                          ///< 水平回転 [rad]
	float m_pitch    = static_cast<float>(40.0_deg); ///< 仰角 [rad]
	float m_distance = 600.0f;                        ///< 注視点からの距離 [m]

	// 追従モード用（setFollowTarget で更新）
	Vec3  m_followPos     = Vec3::Zero();
	float m_followHeading = 0.0f;

	// ホイールクリックドラッグ回転のカーソルアンカー（ドラッグ中は毎フレームここに戻す）
	Point m_dragAnchor = Point{ 0, 0 };

	static constexpr float MOVE_SPEED    = 300.0f;
	static constexpr float ROTATE_SPEED  = 0.005f;
	static constexpr float ZOOM_SPEED    = 0.12f;
	static constexpr float MIN_DIST      = 50.0f;
	static constexpr float MAX_DIST      = 3000.0f;
	static constexpr float MIN_PITCH_DEG = 10.0f;
	static constexpr float MAX_PITCH_DEG = 89.0f;
	/// @brief eye が地形面から最低限浮かせる高さ [m]
	static constexpr float MIN_HEIGHT_ABOVE_TERRAIN = 3.0f;

	/// @brief m_focus / m_yaw / m_pitch / m_distance から m_camera を再構築する
	/// @param world nullptr なら地形床クランプをスキップ
	void rebuild(const World* world = nullptr);

	/// @brief WASD 移動・ホイールクリックドラッグ回転・ホイールズームを処理する
	void handleInput(double dt, const World& world);

	/// @brief 追従モードのカメラを再構築する
	void rebuildFollow();

	/// @brief 一人称モードのカメラを再構築する
	void rebuildFirstPerson();
};
