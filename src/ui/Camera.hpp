#pragma once

/// @brief ゲームカメラ（BasicCamera3D ラッパー）
/// @details WASD 移動・右ドラッグ回転・ホイールズームを提供する
class GameCamera
{
public:
	GameCamera();

	/// @brief 毎フレーム呼ぶ（入力処理・行列更新）
	/// @param dt リアル経過秒
	void update(double dt);

	const BasicCamera3D& camera3D() const { return m_camera; }

	Vec3  focusPoint() const { return m_focus; }
	float distance()   const { return m_distance; }
	float yaw()        const { return m_yaw; }
	float pitch()      const { return m_pitch; }

	/// @brief カメラの視点位置を返す
	Vec3 eyePosition() const
	{
		return m_focus + Vec3{
			Math::Sin(m_yaw)  * Math::Cos(m_pitch),
			Math::Sin(m_pitch),
			Math::Cos(m_yaw)  * Math::Cos(m_pitch)
		} * m_distance;
	}

	/// @brief 注視点を設定する
	void setFocus(Vec3 focus);

	/// @brief スクリーン座標からグラウンド（y=0）上のワールド座標を返す
	/// @return 地面と交差しない場合は none
	Optional<Vec3> screenToGround(Vec2 screenPos) const;

private:
	BasicCamera3D m_camera;

	Vec3  m_focus    = Vec3{ 512.0, 0.0, 512.0 };   ///< 注視点
	float m_yaw      = 0.0f;                          ///< 水平回転 [rad]
	float m_pitch    = static_cast<float>(40.0_deg); ///< 仰角 [rad]
	float m_distance = 600.0f;                        ///< 注視点からの距離 [m]

	static constexpr float MOVE_SPEED    = 300.0f;
	static constexpr float ROTATE_SPEED  = 0.005f;
	static constexpr float ZOOM_SPEED    = 0.12f;
	static constexpr float MIN_DIST      = 50.0f;
	static constexpr float MAX_DIST      = 3000.0f;
	static constexpr float MIN_PITCH_DEG = 10.0f;
	static constexpr float MAX_PITCH_DEG = 89.0f;

	/// @brief m_focus / m_yaw / m_pitch / m_distance から m_camera を再構築する
	void rebuild();

	/// @brief WASD 移動・右ドラッグ回転・ホイールズームを処理する
	void handleInput(double dt);
};
