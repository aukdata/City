#pragma once
#include "../world/World.hpp"

/// @brief カメラモード
enum class CameraMode
{
	Overview,     ///< 俯瞰（デフォルト）
	Follow,       ///< 車両追従（後方視点）
	FirstPerson,  ///< 一人称（車両内視点）
	Capture,      ///< 提出用スクリーンショット撮影
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
	/// @brief Move in the walking camera frame: positive X is screen right, Y is forward.
	void walk(Vec2 input, double distance, const World& world);


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
		return m_camera.getEyePosition();
	}

	/// @brief ワールド座標とカメラ視点との水平距離 (XZ 平面) を返す
	float horizontalDistanceTo(const Float3& worldPos) const
	{
		const Vec3 eye = eyePosition();
		const float dx = worldPos.x - static_cast<float>(eye.x);
		const float dz = worldPos.z - static_cast<float>(eye.z);
		return Math::Sqrt(dx * dx + dz * dz);
	}

	/// @brief 注視点を設定する（初期化用。地形床クランプは適用しない）
	void setFocus(Vec3 focus);

	/// @brief カメラ姿勢を一括設定する（セーブロード用）
	void setState(Vec3 focus, float distance, float yaw, float pitch);

	/// @brief 提出用スクリーンショット撮影カメラを設定する
	void setCaptureState(Vec3 focus, float distance, float yaw, float pitch);

	/// @brief スクリーン座標からグラウンド（y=0）上のワールド座標を返す
	/// @return 地面と交差しない場合は none
	Optional<Vec3> screenToGround(Vec2 screenPos) const;

	/// @brief スクリーン座標からレイを返す
	Ray screenToRay(Vec2 screenPos) const;

	/// @brief パネル上でのマウス入力ブロック設定
	void setBlockInput(bool block) { m_blockInput = block; }

	/// @brief 俯瞰と一人称を切り替える
	void cycleMode()
	{
		if (m_mode == CameraMode::Overview)
		{
			m_mode    = CameraMode::FirstPerson;
			m_fpYaw   = m_yaw + static_cast<float>(Math::Pi);  // 俯瞰の向きを引き継ぐ
			m_fpPitch = 0.0f;
		}
		else
		{
			m_mode = CameraMode::Overview;
		}
	}

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

	// 一人称歩行モード用
	float m_fpYaw   = 0.0f;   ///< 水平方向 [rad]
	float m_fpPitch = 0.0f;   ///< 垂直方向 [rad]

	// ホイールクリックドラッグ回転用
	Point m_dragAnchor    = Point{ 0, 0 };  ///< ドラッグ開始時のカーソル位置（毎フレームここへ戻す）
	Vec3  m_orbitPivot    = Vec3::Zero();   ///< 地形との交点（回転ピボット）
	bool  m_hasOrbitPivot = false;          ///< ピボットが有効かどうか

	static constexpr float MOVE_SPEED    = 300.0f;
	static constexpr float ROTATE_SPEED  = 0.005f;
	static constexpr float ZOOM_SPEED    = 0.12f;
	static constexpr float MIN_DIST      = 5.0f;
	static constexpr float MAX_DIST      = 3000.0f;
	static constexpr float MIN_PITCH_DEG = 10.0f;
	static constexpr float MAX_PITCH_DEG = 89.0f;
	/// @brief eye が地形面から最低限浮かせる高さ [m]
	static constexpr float MIN_HEIGHT_ABOVE_TERRAIN = 3.0f;

	bool m_blockInput = false;  ///< true: ホイール・ミドルクリック操作を無視

	/// @brief m_focus / m_yaw / m_pitch / m_distance から m_camera を再構築する
	/// @param world nullptr なら地形床クランプをスキップ
	void rebuild(const World* world = nullptr);

	/// @brief WASD 移動・ホイールクリックドラッグ回転・ホイールズームを処理する
	void handleInput(double dt, const World& world);

	/// @brief 追従モードのカメラを再構築する
	void rebuildFollow();

	/// @brief 一人称歩行モードの入力処理
	void handleFirstPersonInput(double dt, const World& world);

	/// @brief 一人称モードのカメラを再構築する
	void rebuildFirstPerson();
};
