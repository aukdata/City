#include "Camera.hpp"

GameCamera::GameCamera()
{
	// メンバ変数はヘッダの初期値で初期化済み
	rebuild();
}

void GameCamera::update(double dt, const World& world)
{
	if (m_mode == CameraMode::Overview)
	{
		handleInput(dt, world);
		rebuild(&world);
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

void GameCamera::handleInput(double dt, const World& world)
{
	// ─── WASD 移動 ─────────────────────────────────────────────────────────────
	const Vec3   forward    = Vec3{ -Math::Sin(m_yaw), 0.0, -Math::Cos(m_yaw) };
	const Vec3   right      = Vec3{  Math::Cos(m_yaw), 0.0, -Math::Sin(m_yaw) };
	const double ctrlBoost  = KeyControl.pressed() ? 2.0 : 1.0;
	const double speedScale = static_cast<double>(MOVE_SPEED) * dt * (m_distance / 300.0) * ctrlBoost;

	Vec3 wasdDelta{ 0.0, 0.0, 0.0 };
	if (KeyW.pressed()) wasdDelta += forward * speedScale;
	if (KeyS.pressed()) wasdDelta -= forward * speedScale;
	if (KeyA.pressed()) wasdDelta += right   * speedScale;
	if (KeyD.pressed()) wasdDelta -= right   * speedScale;
	m_focus += wasdDelta;

	// ホイールドラッグ中も WASD が有効になるよう、ピボットも同量移動する
	if (MouseM.pressed() && m_hasOrbitPivot)
		m_orbitPivot += wasdDelta;

	// ─── ホイールクリックドラッグ ─────────────────────────────────────────────
	// Shift なし: 地形交点を中心に回転
	// Shift あり: カメラ平行移動（パン）

	if (MouseM.down())
	{
		m_dragAnchor    = Cursor::Pos();
		m_hasOrbitPivot = false;

		// Shift パン中は回転ピボット計算不要
		if (!KeyShift.pressed())
		{
			const Ray    ray  = screenToRay(Vec2{ m_dragAnchor });
			const Float3 orig = ray.origin.xyz();
			const Float3 dir  = ray.direction.xyz();

			// 下方向成分がなければ地形に当たらない（水平・上向きレイはスキップ）
			if (dir.y < 0.0f)
			{
				constexpr float kStep    = 10.0f;
				constexpr float kMaxDist = 8000.0f;
				float tPrev = 0.0f;

				for (float t = kStep; t < kMaxDist; t += kStep)
				{
					const float px = orig.x + dir.x * t;
					const float pz = orig.z + dir.z * t;
					const float py = orig.y + dir.y * t;

					if (py <= world.sampleHeight(px, pz))
					{
						const float tMid = (tPrev + t) * 0.5f;
						const float hx   = orig.x + dir.x * tMid;
						const float hz   = orig.z + dir.z * tMid;
						const float hy   = world.sampleHeight(hx, hz);

						m_orbitPivot    = Vec3{ hx, hy, hz };
						m_hasOrbitPivot = true;
						break;
					}
					tPrev = t;
				}
			}
		}
	}

	if (MouseM.pressed())
	{
		const Vec2 delta = Cursor::DeltaF();

		if (KeyShift.pressed())
		{
			// ─── Shift + ホイールドラッグ: パン（平行移動）────────────────────────
			// 1 ピクセルあたりのワールド移動量: focus 平面における見かけ上のスケール
			const double panScale = static_cast<double>(m_distance) / Scene::Width()
			                        * 2.0 * Math::Tan(20.0_deg);
			const Vec3 right   = Vec3{  Math::Cos(m_yaw), 0.0, -Math::Sin(m_yaw) };
			const Vec3 fwd     = Vec3{ -Math::Sin(m_yaw), 0.0, -Math::Cos(m_yaw) };
			m_focus += right * delta.x * panScale;
			m_focus += fwd   * delta.y * panScale;

			if (m_hasOrbitPivot)
			{
				m_orbitPivot += right * delta.x * panScale;
				m_orbitPivot += fwd   * delta.y * panScale;
			}
		}
		else
		{
			// ─── ホイールドラッグ: 地形交点を中心に回転 ─────────────────────────
			m_yaw   += static_cast<float>(delta.x) * ROTATE_SPEED;
			m_pitch += static_cast<float>(delta.y) * ROTATE_SPEED;
			m_pitch  = Clamp(
				m_pitch,
				static_cast<float>(Math::ToRadians(MIN_PITCH_DEG)),
				static_cast<float>(Math::ToRadians(MAX_PITCH_DEG)));

			// ピボット P を dragAnchor の画面位置に固定する補正
			if (m_hasOrbitPivot)
			{
				rebuild(&world);  // 仮ビルド（update() でもう一度ビルドされる）

				const Ray    ray = screenToRay(Vec2{ m_dragAnchor });
				const Float3 co  = ray.origin.xyz();
				const Float3 cd  = ray.direction.xyz();

				if (Math::Abs(cd.y) > 1e-6f)
				{
					const float t = (static_cast<float>(m_orbitPivot.y) - co.y) / cd.y;
					if (t > 0.0f)
					{
						const Vec3 Q{
							static_cast<double>(co.x + cd.x * t),
							m_orbitPivot.y,
							static_cast<double>(co.z + cd.z * t)
						};
						m_focus += m_orbitPivot - Q;
					}
				}
			}
		}

		// カーソルをドラッグ開始位置に固定し、ウィンドウ外へ出るのを防ぐ
		Cursor::SetPos(m_dragAnchor);
		Cursor::RequestStyle(CursorStyle::Hidden);
	}

	// ─── ホイールズーム（Ctrl 押下中はスキップ：地形編集ブラシサイズ変更に使用）───
	const double wheel = Mouse::Wheel();
	if (wheel != 0.0 && !KeyControl.pressed())
	{
		m_distance = static_cast<float>(
			Clamp(
				static_cast<double>(m_distance) * (1.0 + wheel * ZOOM_SPEED),
				static_cast<double>(MIN_DIST),
				static_cast<double>(MAX_DIST)));
	}

	// ─── Numpad0: 真上視点リセット ───────────────────────────────────────────
	if (KeyNum0.down())
	{
		m_pitch = static_cast<float>(Math::ToRadians(89.0));
		m_yaw   = 0.0f;
	}
}

void GameCamera::rebuild(const World* world)
{
	Vec3 eye = m_focus + Vec3{
		Math::Sin(m_yaw)  * Math::Cos(m_pitch),
		Math::Sin(m_pitch),
		Math::Cos(m_yaw)  * Math::Cos(m_pitch)
	} * m_distance;

	// 地形床クランプ: eye が地形面より低い場合は浮かせる
	if (world)
	{
		const float terrainY = world->sampleHeight(
			static_cast<float>(eye.x), static_cast<float>(eye.z));
		if (eye.y < terrainY + MIN_HEIGHT_ABOVE_TERRAIN)
			eye.y = terrainY + MIN_HEIGHT_ABOVE_TERRAIN;
	}

	m_camera = BasicCamera3D{ Scene::Size(), 40_deg, eye, m_focus };
}

void GameCamera::setFocus(Vec3 focus)
{
	m_focus = focus;
	rebuild();  // 初期化用：地形床クランプなし
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

Ray GameCamera::screenToRay(Vec2 screenPos) const
{
	return m_camera.screenToRay(screenPos);
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
