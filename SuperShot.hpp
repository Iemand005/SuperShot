#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <EditableGame.hpp>
#include <Object.hpp>
#include <Character.hpp>
#include <Primitives.hpp>
#include <vr/OpenXR.hpp>

#include "ChunkManager.hpp"
#ifdef FE_HAS_VULKAN
#include "ChunkMesher.hpp"
#include "ChunkBatcher.hpp"
#endif

class SuperFlight : public fe::EditableGame
{
public:
	explicit SuperFlight(fe::XRGameOptions options) : fe::EditableGame(options)
	{
		SetClearColor(0.05f, 0.07f, 0.12f);
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
		LoadShaders("resources/shaders/VertexShader_gles300.glsl",
			"resources/shaders/FragmentShader_gles300.glsl");
#else
		if (!options.useVulkan)
			LoadShaders("resources/shaders/VertexShader_gl330.glsl",
				"resources/shaders/FragmentShader_gl330.glsl");
#endif
#ifdef FE_HAS_VULKAN
		if (options.useVulkan)
		{
			LoadShaders<fe::VertexArray>("resources/shaders/VertexShader_vk_array.spv", "resources/shaders/FragmentShader_vk_array.spv");
			LoadShaders<VoxelPackedVertex>("resources/shaders/VertexShader_vk_foxcraft.spv", "resources/shaders/FragmentShader_vk_array.spv");
			chunkBatcher_ = std::make_unique<ChunkBatcher>(
				static_cast<fe::VulkanDevice *>(renderDevice.get()),
				ChunkMesher::BuildBlockTextureImages());
			chunkManager->SetBatcher(chunkBatcher_.get());
			chunkManager->SetUseBatcherPath(true);
		}
#endif
		camera->SetPos(glm::vec3(0.0f, 70.0f, 0.0f));
		camera->LookAt(glm::vec3(0.0f, 70.0f, -10.0f));
	}

	void Init() override
	{
		auto *window = getWindow<fe::SDLWindow>();
		window->Show();
		window->DisableVSync();
		camera->SetAspect(window->width, window->height);
		SetupWorld();
		window->StartMouseCapture();
	}

	void Step() override
	{
		ProcessInput();
		const float dt = static_cast<float>(fpsCounter.deltaTime);
		auto *window = getWindow<fe::SDLWindow>();

		WalkingUpdate(window, dt);

		if (window->IsKeyDown(SDL_SCANCODE_B)) CreateBall();

		UpdateLoadedChunks();
		chunkManager->UpdatePausedState(playerCenter_, glm::normalize(glm::vec2(camera->front.x, camera->front.z)));
		chunkManager->Update(1, GetPhysicsFactory(), scene.get(), playerCenter_, chunkLoadDistance, physicsDistance);
		UpdateTablePlacement();
		Update();


		HandleXRInputs();
		UpdateControllerCubes(dt);

		RespawnIfFell();
		ApplyVREyeHeight();
		SyncCameraToPlayer();
		Redraw();
	}

	void SetupWorld()
	{
		if (auto *physics = GetPhysicsFactory())
			physics->SetGravity(glm::vec3(0.0f, -20.0f, 0.0f));

		CreatePlayer();
		CreateControllerCubes();
		CreateBall();
		CreateThrowingLane();
	}

	std::shared_ptr<fe::Object> AddCube(const char *name, glm::vec3 color, float size, glm::vec3 position)
	{
		auto object = std::make_shared<fe::Object>(fe::Primitives::GenerateCube(size));
		object->name = name;
		object->color = color;
		object->state.position = position;
		object->isStatic = true;
		scene->AddObject(object);
		return object;
	}

	std::shared_ptr<fe::Object> AddSolidBox(const char *name, glm::vec3 color, glm::vec3 size, glm::vec3 position)
	{
		auto object = AddCube(name, color, 1.0f, position);
		object->state.scale = size;
		if (auto *physics = GetPhysicsFactory())
		{
			auto body = physics->CreateObject(size, false, false);
			if (body)
			{
				body->SetPosition(position);
				object->SetPhysicsObject(std::move(body));
			}
		}
		return object;
	}

	std::shared_ptr<fe::Object> AddSphere(const char *name, glm::vec3 color, glm::vec3 position)
	{
		auto object = std::make_shared<fe::Object>(fe::Primitives::GenerateSphere(ballRadius_, 24, 16));
		object->name = name;
		object->color = color;
		object->state.position = position;
		object->isStatic = false;
		if (auto *physics = GetPhysicsFactory())
		{
			auto body = physics->CreateSphereObject(ballRadius_, true);
			if (body)
			{
				body->SetPosition(position);
				body->SetLinearVelocity(glm::vec3(0.0f));
				body->SetAngularVelocity(glm::vec3(0.0f));
			}
			object->SetPhysicsObject(std::move(body));
		}
		scene->AddObject(object);
		return object;
	}

	void CreateThrowingLane()
	{
		const glm::vec3 base(0.0f, 0.0f, -24.0f);
		struct Part { const char *name; glm::vec3 color; glm::vec3 size; glm::vec3 offset; };
		const Part parts[] = {
			{"ThrowingLaneFloor", {0.38f, 0.40f, 0.43f}, {15.0f, 0.2f, 80.0f}, {0.0f, 30.1f, 0.10f}},
			{"ThrowingLaneLeftWall", {0.26f, 0.30f, 0.34f}, {0.2f, 0.8f, 80.0f}, {-7.4f, 30.4f, 0.0f}},
			{"ThrowingLaneRightWall", {0.26f, 0.30f, 0.34f}, {0.2f, 0.8f, 80.0f}, {7.4f, 30.4f, 0.0f}},
		};
		for (const auto &part : parts)
		{
			auto object = AddCube(part.name, part.color, 1.0f, base + part.offset);
			object->state.scale = part.size;
		}
	}

	void CreateTable()
	{
		if (!tableParts_.empty())
			return;

		const glm::vec3 topSize(1.6f, 0.12f, 0.9f);
		const float legHeight = 0.84f;
		const float legSize = 0.1f;
		const glm::vec3 topOffset(0.0f, legHeight + topSize.y * 0.5f, 0.0f);

		auto addPart = [&](const char *name, glm::vec3 color, glm::vec3 size, glm::vec3 offset) {
			auto object = AddSolidBox(name, color, size, glm::vec3(0.0f));
			tableParts_.push_back({object, offset});
		};

		addPart("TableTop", {0.55f, 0.38f, 0.22f}, topSize, topOffset);
		const float legX[2] = {-0.65f, 0.65f};
		const float legZ[2] = {-0.32f, 0.32f};
		for (float x : legX)
			for (float z : legZ)
				addPart("TableLeg", {0.35f, 0.24f, 0.14f},
					glm::vec3(legSize, legHeight, legSize),
					glm::vec3(x, legHeight * 0.5f, z));
	}

	void PlaceTable(float floorY)
	{
		CreateTable();

		const glm::vec3 base(0.0f, floorY, 8.4f);
		for (auto &part : tableParts_)
		{
			const glm::vec3 position = base + part.offset;
			part.object->SetPosition(position);
			if (part.object->physicsObject)
				part.object->physicsObject->SetPosition(position);
		}
		tableFloorY_ = floorY;
	}

	void UpdateTablePlacement()
	{
		const int surface = std::max({
			chunkManager->GetSurfaceHeight(-1, 8),
			chunkManager->GetSurfaceHeight(0, 8),
			chunkManager->GetSurfaceHeight(-1, 9),
			chunkManager->GetSurfaceHeight(0, 9)});
		if (surface <= 0)
			return;

		const float floorY = static_cast<float>(surface);
		if (!tableParts_.empty() && glm::abs(floorY - tableFloorY_) < 0.01f)
			return;

		PlaceTable(floorY);
	}

	void CreateControllerCubes()
	{
		const auto input = GetWorldInputState();
		const char *names[2] = {"LeftController", "RightController"};
		const glm::vec3 colors[2] = {{0.2f, 0.8f, 1.0f}, {1.0f, 0.5f, 0.1f}};
		const glm::vec3 offsets[2] = {{-0.25f, 1.2f, -0.3f}, {0.25f, 1.2f, -0.3f}};
		const bool active[2] = {input.leftGripPoseActive, input.rightGripPoseActive};
		for (size_t i = 0; i < 2; ++i)
		{
			hands_[i].cube = AddCube(names[i], colors[i], 0.08f, playerSpawn + offsets[i]);
			hands_[i].cube->visible = active[i];
		}
	}

	void CreateHandJointCubes()
	{
		// constexpr size_t count = static_cast<size_t>(fe::OpenXR::HandJoint::Count);
		// handJointCubes_.reserve(count * 2);
		// const glm::vec3 colors[2] = {{0.2f, 0.8f, 1.0f}, {1.0f, 0.5f, 0.1f}};
		// const char *names[2] = {"Left", "Right"};
		// for (size_t hand = 0; hand < 2; ++hand)
		// 	for (size_t joint = 0; joint < count; ++joint)
		// 	{
		// 		auto cube = AddCube(
		// 			(std::string(names[hand]) + "HandJoint" + std::to_string(joint)).c_str(),
		// 			colors[hand], 0.012f, glm::vec3(0.0f));
		// 		cube->visible = false;
		// 		handJointCubes_.push_back(cube);
		// 	}
	}

	void UpdateHandJointCubes(const fe::OpenXR::InputState &input)
	{
		// if (handJointCubes_.empty() && GetOpenXR().IsHandTrackingSupported())
		// 	CreateHandJointCubes();
		// if (handJointCubes_.empty())
		// 	return;

		// constexpr size_t count = static_cast<size_t>(fe::OpenXR::HandJoint::Count);
		// const fe::OpenXR::HandState *hands[2] = {&input.leftHand, &input.rightHand};
		// for (size_t hand = 0; hand < 2; ++hand)
		// 	for (size_t joint = 0; joint < count; ++joint)
		// 	{
		// 		auto &cube = handJointCubes_[hand * count + joint];
		// 		const auto &pose = hands[hand]->joints[joint];
		// 		cube->visible = hands[hand]->active && pose.active;
		// 		if (cube->visible)
		// 		{
		// 			cube->SetPosition(pose.position);
		// 			cube->SetOrientation(pose.orientation);
		// 		}
		// 	}
	}

	void CreateBall()
	{
		ball_ = AddSphere("XRBall", {0.95f, 0.85f, 0.2f}, playerSpawn + glm::vec3(0.0f, 1.05f, -0.55f));
		grabbable_.push_back(ball_);
	}

	void HandleXRInputs()
	{
		if (GetOpenXR().ConsumeButtonPress(fe::OpenXR::Button::X))
			SpawnSphere();
	}

	void SpawnSphere()
	{
		glm::vec3 forward = camera->front;
		forward.y = 0.0f;
		if (glm::length(forward) < 1e-4f)
			forward = glm::vec3(0.0f, 0.0f, -1.0f);
		forward = glm::normalize(forward);

		const glm::vec3 base = player ? player->state.position : playerSpawn;
		const glm::vec3 position = base + glm::vec3(0.0f, 1.5f, 0.0f) + forward * 0.8f;
		auto sphere = AddSphere("XRSphere", {0.3f, 0.85f, 0.45f}, position);
		grabbable_.push_back(sphere);
		GetOpenXR().TriggerHaptic(fe::OpenXR::Hand::Left, 0.5f, 0.08f);
	}

	static fe::OpenXR::Hand XRHand(size_t index)
	{
		return index == 0 ? fe::OpenXR::Hand::Left : fe::OpenXR::Hand::Right;
	}

	void UpdateControllerCubes(float dt)
	{
		const auto input = GetWorldInputState();
		UpdateHandJointCubes(input);

		const glm::vec3 gripPos[2] = {input.leftGripPosition, input.rightGripPosition};
		const glm::quat gripRot[2] = {input.leftGripOrientation, input.rightGripOrientation};
		const bool poseActive[2] = {input.leftGripPoseActive, input.rightGripPoseActive};
		auto &xr = GetOpenXR();
		const bool gripDown[2] = {
			xr.IsGripPressed(fe::OpenXR::Hand::Left), xr.IsGripPressed(fe::OpenXR::Hand::Right)};
		const float cubeExtent = 0.08f;

		auto findTouched = [&](const glm::vec3 &pos, float scale) {
			std::shared_ptr<fe::Object> best;
			float bestDist = std::numeric_limits<float>::max();
			for (const auto &object : grabbable_)
			{
				if (!object)
					continue;
				const float reach = cubeExtent * scale + ballRadius_ * object->state.scale.x;
				const float dist = glm::length(pos - object->state.position);
				if (dist <= reach && dist < bestDist)
				{
					best = object;
					bestDist = dist;
				}
			}
			return best;
		};

		for (size_t i = 0; i < 2; ++i)
		{
			auto &hand = hands_[i];
			if (!poseActive[i])
			{
				hand.hasPrev = false;
				hand.velocity = glm::vec3(0.0f);
				hand.angularVelocity = glm::vec3(0.0f);
			}
			else
			{
				if (hand.hasPrev && dt > 1e-5f)
				{
					hand.velocity = glm::mix(hand.velocity, (gripPos[i] - hand.prevPos) / dt, 0.75f);
					glm::quat delta = gripRot[i] * glm::inverse(hand.prevRot);
					if (delta.w < 0.0f)
						delta = glm::quat(-delta.w, -delta.x, -delta.y, -delta.z);
					const float angle = 2.0f * std::acos(std::clamp(delta.w, -1.0f, 1.0f));
					const glm::vec3 axis(delta.x, delta.y, delta.z);
					const float axisLength = glm::length(axis);
					hand.angularVelocity = (axisLength > 1e-6f && angle > 1e-6f)
						? glm::mix(hand.angularVelocity, axis / axisLength * (angle / dt), 0.5f)
						: glm::vec3(0.0f);
				}
				hand.prevPos = gripPos[i];
				hand.prevRot = gripRot[i];
				hand.hasPrev = true;
			}

			if (!hand.cube)
				continue;
			hand.cube->visible = poseActive[i];
			if (!poseActive[i])
			{
				hand.touching = false;
				continue;
			}
			hand.cube->SetPosition(gripPos[i]);
			hand.cube->SetOrientation(gripRot[i]);

			const bool holding = grabbed_ && grabbedHand_ == static_cast<int>(i);
			const bool hit = holding || findTouched(gripPos[i], hand.scale) != nullptr;
			if (hit && !holding)
			{
				hand.scale = std::min(hand.scale + 0.2f * dt, 1.35f);
				if (!hand.touching)
					xr.TriggerHaptic(XRHand(i), 0.7f, 0.08f);
			}
			else if (!hit)
			{
				hand.scale = std::max(hand.scale - 0.5f * dt, 1.0f);
			}
			hand.touching = hit;
			hand.cube->SetScale(glm::vec3(hand.scale));
		}

		if (grabbed_)
		{
			const size_t i = static_cast<size_t>(grabbedHand_);
			if (!gripDown[i] || !poseActive[i])
				ReleaseGrabbedObject();
			else
			{
				const glm::vec3 position = gripPos[i] + gripRot[i] * grabOffset_;
				const glm::quat rotation = gripRot[i] * grabRotation_;
				grabbed_->SetPosition(position);
				grabbed_->SetOrientation(rotation);
				if (grabbed_->physicsObject)
				{
					grabbed_->physicsObject->SetPosition(position);
					grabbed_->physicsObject->SetRotation(rotation);
				}
			}
			return;
		}

		for (size_t i = 0; i < 2; ++i)
			if (gripDown[i] && poseActive[i])
				if (auto object = findTouched(gripPos[i], hands_[i].scale))
				{
					GrabObject(object, static_cast<int>(i), gripPos[i], gripRot[i]);
					break;
				}
	}

	void GrabObject(const std::shared_ptr<fe::Object> &object, int handIndex,
		const glm::vec3 &handPos, const glm::quat &handRot)
	{
		if (!object)
			return;
		const glm::quat inverse = glm::inverse(handRot);
		grabbed_ = object;
		grabbedHand_ = handIndex;
		grabOffset_ = inverse * (object->state.position - handPos);
		grabRotation_ = inverse * object->state.orientation;
		grabWasStatic_ = object->isStatic;
		object->isStatic = true;
		GetOpenXR().TriggerHaptic(XRHand(static_cast<size_t>(handIndex)), 0.8f, 0.1f);
	}

	void ReleaseGrabbedObject()
	{
		if (!grabbed_)
			return;
		if (grabbed_->physicsObject)
		{
			grabbed_->physicsObject->SetPosition(grabbed_->state.position);
			grabbed_->physicsObject->SetRotation(grabbed_->state.orientation);
			const size_t i = static_cast<size_t>(grabbedHand_);
			glm::vec3 throwVelocity = hands_[i].velocity * 2.5f;
			const float speed = glm::length(throwVelocity);
			if (speed > 45.0f)
				throwVelocity = throwVelocity / speed * 45.0f;
			glm::vec3 spin = hands_[i].angularVelocity;
			const float spinLength = glm::length(spin);
			if (spinLength > 30.0f)
				spin = spin / spinLength * 30.0f;
			grabbed_->physicsObject->SetLinearVelocity(throwVelocity);
			grabbed_->physicsObject->SetAngularVelocity(spin);
		}
		grabbed_->isStatic = grabWasStatic_;
		grabbed_.reset();
		grabbedHand_ = -1;
	}

	void CreatePlayer()
	{
		player = std::make_shared<fe::Character>();
		scene->AddObject(player);
		player->state.position = playerSpawn;
		player->gravityEnabled = true;
		player->jumpSpeed = 7.07f;
		player->moveSpeed = walkSpeed;
		if (auto *physics = GetPhysicsFactory())
		{
			if (auto body = physics->CreateCharacter(1.875f, 0.3f, player->state.position, false))
			{
				player->SetPhysicsCharacter(std::move(body));
				player->physicsCharacter->SetPosition(player->state.position);
				player->physicsCharacter->SetJumpSpeed(player->jumpSpeed);
			}
		}
		UpdateLoadedChunks();
		ApplyVREyeHeight();
		SyncCameraToPlayer();
	}

	void ApplyVREyeHeight()
	{
		const float centerOffset = player && player->physicsCharacter
			? player->physicsCharacter->GetCenterOffset()
			: playerHeight * 0.5f;
		positionOffset.y = playerHeight - centerOffset;
	}

	void UpdateLoadedChunks()
	{
		const glm::vec3 position = camera->GetPos();
		playerCenter_ = {
			static_cast<int>(std::floor(position.x / static_cast<float>(Chunk::WIDTH))),
			static_cast<int>(std::floor(position.z / static_cast<float>(Chunk::DEPTH)))};
		const int distance = chunkLoadDistance + chunkOutgenDistance;
		chunkManager->LoadChunksInsideRange(playerCenter_, distance,
			glm::normalize(glm::vec2(camera->front.x, camera->front.z)));
		chunkManager->UnloadChunksOutsideRange(playerCenter_, distance);
	}

	void WalkingUpdate(fe::SDLWindow *window, float)
	{
		if (!player)
			return;
		player->moveSpeed = window->IsKeyDown(SDL_SCANCODE_LCTRL) ? sprintSpeed : walkSpeed;
		const std::pair<SDL_Scancode, fe::Direction> moves[] = {
			{SDL_SCANCODE_W, fe::Direction::Forwards}, {SDL_SCANCODE_S, fe::Direction::Backwards},
			{SDL_SCANCODE_A, fe::Direction::Left}, {SDL_SCANCODE_D, fe::Direction::Right},
			{SDL_SCANCODE_SPACE, fe::Direction::Up}, {SDL_SCANCODE_LSHIFT, fe::Direction::Down}};
		for (const auto &move : moves)
			if (window->IsKeyDown(move.first))
				player->Move(move.second, camera.get());
	}

	void SyncCameraToPlayer()
	{
		if (!player)
			return;
		const float height = player->physicsCharacter ? player->physicsCharacter->GetHeight() : 1.5f;
		camera->SetPos(player->state.position + glm::vec3(0.0f, 0.4f * height, 0.0f));
	}

	void RespawnIfFell()
	{
		if (!player || !player->physicsCharacter)
			return;
		glm::vec3 position = player->physicsCharacter->GetPosition();
		if (position.y >= -5.0f)
			return;
		const int surface = chunkManager->GetSurfaceHeight(
			static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
		if (surface <= 0)
			return;
		position.y = static_cast<float>(surface) + 1.2f;
		player->physicsCharacter->SetPosition(position);
		player->physicsCharacter->SetLinearVelocity(glm::vec3(0.0f));
		player->state.position = position;
	}

	void OnDraw() override
	{
		fe::EditableGame::OnDraw();
#ifdef FE_HAS_VULKAN
		if (chunkBatcher_ && useVulkan)
		{
			chunkBatcher_->Update(camera->GetPos());
			chunkBatcher_->Draw();
		}
#endif
	}

	void ProcessInput()
	{
		SDL_Event event;
		auto *window = getWindow<fe::SDLWindow>();
		auto look = [&](float dx, float dy) {
			camera->yaw += dx * 0.1f;
			camera->pitch -= dy * 0.1f;
			camera->UpdateDirection();
			camera->pitch = std::clamp(camera->pitch, -89.0f, 89.0f);
		};

		while (window->PollSDLEvent(&event))
		{
			ImGui_ImplSDL3_ProcessEvent(&event);
			const bool uiCaptures = ImGui::GetIO().WantCaptureMouse;
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				window->PrepareClose();
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT && !uiCaptures)
				{
					if (!window->IsCapturingMouse())
						window->StartMouseCapture();
				}
				break;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				Redraw();
				break;
			case SDL_EVENT_MOUSE_MOTION:
				if (event.motion.which != SDL_TOUCH_MOUSEID && window->IsCapturingMouse())
					look(static_cast<float>(event.motion.xrel), static_cast<float>(event.motion.yrel));
				break;
			case SDL_EVENT_FINGER_DOWN:
				if (event.tfinger.fingerID == 0 && !uiCaptures && !window->IsCapturingMouse())
					window->StartMouseCapture();
				break;
			case SDL_EVENT_FINGER_MOTION:
				if (event.tfinger.fingerID == 0 && window->IsCapturingMouse())
					look(event.tfinger.dx * window->width, event.tfinger.dy * window->height);
				break;
			case SDL_EVENT_FINGER_UP:
				if (event.tfinger.fingerID == 0 && !uiCaptures)
					window->StopMouseCapture();
				break;
			case SDL_EVENT_KEY_DOWN:
				switch (event.key.key)
				{
				case SDLK_F11: window->ToggleFullscreen(); break;
				case SDLK_F3: showDebugUI = !showDebugUI; break;
				case SDLK_F2: window->StartMouseCapture(); break;
				case SDLK_F4: ToggleXR(); break;
				default: break;
				}
				break;
			default: break;
			}
		}
		if (window->IsKeyDown(SDL_SCANCODE_ESCAPE) || ImGui::GetIO().WantCaptureMouse)
			window->StopMouseCapture();
	}

	void initUI() override {}

	void drawUI() override
	{
		const bool showXR = IsInstanceValid();
		if (!showDebugUI && !showXR)
			return;
		BeginFrame();

		if (showXR)
			DrawXRUI();
		if (showDebugUI)
			DrawDebugUI();

		fe::EditableGame::DrawDebugUI();

		EndFrame();
	}

	void DrawXRUI()
	{
		const auto input = GetWorldInputState();
		auto &xr = GetOpenXR();
		ImGui::Begin("XR Controllers");

		const char *poseNames[4] = {"Left grip", "Left aim", "Right grip", "Right aim"};
		const glm::vec3 positions[4] = {
			input.leftGripPosition, input.leftAimPosition, input.rightGripPosition, input.rightAimPosition};
		const glm::quat rotations[4] = {
			input.leftGripOrientation, input.leftAimOrientation,
			input.rightGripOrientation, input.rightAimOrientation};
		const bool active[4] = {
			input.leftGripPoseActive, input.leftAimPoseActive,
			input.rightGripPoseActive, input.rightAimPoseActive};
		for (size_t i = 0; i < 4; ++i)
		{
			if (active[i])
				ImGui::Text("%s  x %.3f  y %.3f  z %.3f", poseNames[i], positions[i].x, positions[i].y, positions[i].z);
			else
				ImGui::TextDisabled("%s  (not tracked)", poseNames[i]);
		}
		ImGui::Separator();
		for (size_t i = 0; i < 4; ++i)
		{
			if (!active[i])
				continue;
			const glm::quat &q = rotations[i];
			ImGui::Text("%s rot  %.2f %.2f %.2f %.2f", poseNames[i], q.x, q.y, q.z, q.w);
		}
		ImGui::Separator();
		ImGui::Text("Buttons");
		const char *buttonNames[8] = {"X (left, spawn)", "Y (left)", "A (right)", "B (right)",
			"Left trigger", "Right trigger", "Left squeeze", "Right squeeze"};
		const bool buttons[8] = {
			xr.IsButtonHeld(fe::OpenXR::Button::X), xr.IsButtonHeld(fe::OpenXR::Button::Y),
			xr.IsButtonHeld(fe::OpenXR::Button::A), xr.IsButtonHeld(fe::OpenXR::Button::B),
			xr.IsButtonHeld(fe::OpenXR::Button::LeftTrigger), xr.IsButtonHeld(fe::OpenXR::Button::RightTrigger),
			xr.IsGripPressed(fe::OpenXR::Hand::Left), xr.IsGripPressed(fe::OpenXR::Hand::Right)};
		for (size_t i = 0; i < 8; ++i)
			if (buttons[i])
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s: DOWN", buttonNames[i]);
			else
				ImGui::TextDisabled("%s: up", buttonNames[i]);
		ImGui::Text("Holding: %s (%s)",
			grabbed_ ? grabbed_->name.c_str() : "none",
			grabbedHand_ < 0 ? "-" : (grabbedHand_ == 0 ? "left" : "right"));
		ImGui::End();
	}

	void DrawDebugUI()
	{
		ImGui::Begin("SuperFlight");

		if (ImGui::CollapsingHeader("Terrain"))
		{
			ImGui::DragInt("Render Distance", &chunkLoadDistance);
			ImGui::DragInt("Physics Distance", &physicsDistance, 0.5f, 0, 20);
			ImGui::SliderInt("Terrain Pre-gen", &chunkOutgenDistance, 1, 10);
		}
		if (ImGui::CollapsingHeader("Player"))
		{
			ImGui::SliderFloat("VR Eye Height", &playerHeight, 1.0f, 2.2f, "%.2f m");
			ImGui::Text("VR origin offset: %.2f m", positionOffset.y);
		}
		ImGui::End();
	}

private:
	fe::OpenXR::InputState GetWorldInputState() const
	{
		const glm::vec3 origin = player ? player->state.position : glm::vec3(0.0f);
		const glm::quat rotation = glm::angleAxis(playerYaw, glm::vec3(0.0f, 1.0f, 0.0f));
		return GetOpenXR().GetWorldInputState(origin + positionOffset, rotation);
	}

	bool showDebugUI = false;

	struct HandRig
	{
		std::shared_ptr<fe::Object> cube;
		float scale = 1.0f;
		bool touching = false;
		glm::vec3 prevPos{0.0f};
		glm::quat prevRot{1.0f, 0.0f, 0.0f, 0.0f};
		bool hasPrev = false;
		glm::vec3 velocity{0.0f};
		glm::vec3 angularVelocity{0.0f};
	};

	struct TablePart
	{
		std::shared_ptr<fe::Object> object;
		glm::vec3 offset{0.0f};
	};

	std::vector<TablePart> tableParts_;
	float tableFloorY_ = -1.0f;

	const glm::vec3 playerSpawn{0.0f, 33.0f, 10.0f};

	float walkSpeed = 25.0f;
	float sprintSpeed = 40.0f;

	std::unique_ptr<ChunkManager> chunkManager = std::make_unique<ChunkManager>(4);
#ifdef FE_HAS_VULKAN
	std::unique_ptr<ChunkBatcher> chunkBatcher_;
#endif
	glm::ivec2 playerCenter_{0, 0};
	int chunkLoadDistance = 6;
	int chunkOutgenDistance = 1;
	int physicsDistance = 2;

	HandRig hands_[2];
	std::vector<std::shared_ptr<fe::Object>> handJointCubes_;
	std::shared_ptr<fe::Object> ball_;
	std::vector<std::shared_ptr<fe::Object>> grabbable_;
	float ballRadius_ = 0.25f;
	std::shared_ptr<fe::Object> grabbed_;
	int grabbedHand_ = -1;
	glm::vec3 grabOffset_{0.0f};
	glm::quat grabRotation_{1.0f, 0.0f, 0.0f, 0.0f};
	bool grabWasStatic_ = false;
};
