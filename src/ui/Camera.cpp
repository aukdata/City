#include "Camera.hpp"

GameCamera::GameCamera()
{
	// メンバ変数はヘッダの初期値で初期化済み
	rebuild();
}

void GameCamera::update(double dt)
{
	if (m_mode == CameraMode::Overview)
	{
		handleInput(dt);
		rebuild();
	}
	else if (m_mode == CameraMode::Follow)
	{
		// ホイールズームで追従距離調整は不要。yaw 調整のみ許容
		if (MouseM.pressed())
		{
			const Vec2 delta = Cursor::DeltaF();
			m_followHeading -= static_cast<float>(delta.x) * ROTATE_SPEED * 4.0f;
		}
		rebuildFollow();
		// Overview に戻ったとき focus を現在の追従位置にリセット
		m_focus = m_followPos;
	}
	else // FirstPerson
	{
		rebuildFirstPerson();
		m_focus = m_followPos;
	}
}

void GameCamera::handleInput(double dt)
{
	// WASD 移動（forward = カメラ視線の水平成分 = focus - eye の XZ 正規化）
	const Vec3 forward = Vec3{ -Math::Sin(m_yaw), 0.0, -Math::Cos(m_yaw) };
	const Vec3 right   = Vec3{  Math::Cos(m_yaw), 0.0, -Math::Sin(m_yaw) };
	const double speedScale = static_cast<double>(MOVE_SPEED) * dt * (m_distance / 300.0);

	if (KeyW.pressed()) m_focus += forward * speedScale;
	if (KeyS.pressed()) m_focus -= forward * speedScale;
	if (KeyA.pressed()) m_focus += right   * speedScale;
	if (KeyD.pressed()) m_focus -= right   * speedScale;

	// ホイールクリック（中ボタン）ドラッグ: 回転
	if (MouseM.pressed())
	{
		const Vec2 delta = Cursor::DeltaF();
		m_yaw   += static_cast<float>(delta.x) * ROTATE_SPEED;
		m_pitch += static_cast<float>(delta.y) * ROTATE_SPEED;
		m_pitch = Clamp(
			m_pitch,
			static_cast<float>(Math::ToRadians(MIN_PITCH_DEG)),
			static_cast<float>(Math::ToRadians(MAX_PITCH_DEG)));
	}

	// ホイールズーム（Ctrl 押下中はスキップ：地形編集ブラシサイズ変更に使用）
	const double wheel = Mouse::Wheel();
	if (wheel != 0.0 && !KeyControl.pressed())
	{
		m_distance = static_cast<float>(
			Clamp(
				static_cast<double>(m_distance) * (1.0 + wheel * ZOOM_SPEED),
				static_cast<double>(MIN_DIST),
				static_cast<double>(MAX_DIST)));
	}

	// Numpad0: 真上視点リセット
	if (KeyNum0.down())
	{
		m_pitch = static_cast<float>(Math::ToRadians(89.0));
		m_yaw   = 0.0f;
	}
}

void GameCamera::rebuild()
{
	const Vec3 eye = m_focus + Vec3{
		Math::Sin(m_yaw)  * Math::Cos(m_pitch),
		Math::Sin(m_pitch),
		Math::Cos(m_yaw)  * Math::Cos(m_pitch)
	} * m_distance;

	m_camera = BasicCamera3D{ Scene::Size(), 40_deg, eye, m_focus };
}

void GameCamera::setFocus(Vec3 focus)
{
	m_focus = focus;
	rebuild();
}

Optional<Vec3> GameCamera::screenToGround(Vec2 screenPos) const
{
	const Ray ray = m_camera.screenToRay(screenPos);

	// y=0 の無限平面（通過点 = 原点, 法線 = (0,1,0)）と交差判定する
	if (const auto hit = ray.intersectsAt(InfinitePlane{ Float3{ 0, 0, 0 }, Float3{ 0, 1, 0 } }))
	{
		const Vec3 pos{ *hit };
		if (IsFinite(pos.x) && IsFinite(pos.z))
			return pos;
	}

	return none;
}

void GameCamera::setFollowTarget(Vec3 pos, float heading)
{
	m_followPos     = pos;
	m_followHeading = heading;
}

void GameCamera::rebuildFollow()
{
	// 車両の後方 25m・高さ 10m からターゲットを見る
	const Vec3 back = Vec3{
		-Math::Sin(m_followHeading),
		0.0,
		-Math::Cos(m_followHeading)
	};
	const Vec3 eye    = m_followPos + back * 25.0 + Vec3{ 0, 10, 0 };
	const Vec3 target = m_followPos + Vec3{ 0, 2, 0 };
	m_camera = BasicCamera3D{ Scene::Size(), 60_deg, eye, target };
}

void GameCamera::rebuildFirstPerson()
{
	// 運転席視点（車両位置から 1.3m 上）
	const Vec3 eye = m_followPos + Vec3{ 0, 1.3, 0 };
	const Vec3 fwd = Vec3{
		Math::Sin(m_followHeading),
		0.0,
		Math::Cos(m_followHeading)
	};
	const Vec3 target = eye + fwd * 10.0;
	m_camera = BasicCamera3D{ Scene::Size(), 80_deg, eye, target };
}
