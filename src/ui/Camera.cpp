#include "Camera.hpp"

GameCamera::GameCamera()
{
	// メンバ変数はヘッダの初期値で初期化済み
	rebuild();
}

void GameCamera::update(double dt)
{
	handleInput(dt);
	rebuild();
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

	// ホイールズーム
	const double wheel = Mouse::Wheel();
	if (wheel != 0.0)
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

	// y=0 の無限平面（法線 = (0,1,0), 通過点 = 原点）と交差判定する
	if (const auto hit = ray.intersectsAt(InfinitePlane{ Float3{ 0, 1, 0 }, Float3{ 0, 0, 0 } }))
		return Vec3{ *hit };

	return none;
}
