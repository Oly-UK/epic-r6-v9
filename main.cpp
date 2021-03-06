#include "includes.hpp"
#include "memory.hpp"
#include "game.hpp"
#include "renderer.hpp"

//defining globals
const unsigned int g_screen_width  = GetSystemMetrics(SM_CXSCREEN);
const unsigned int g_screen_height = GetSystemMetrics(SM_CYSCREEN);

HWND g_hwnd, g_game_hwnd;

DWORD g_process_id;
HANDLE g_process_handle;
MODULEENTRY32 g_module;

uintptr_t g_game_man_offset, g_prof_man_offset;
uintptr_t g_game_manager, g_entity_list, g_camera;
view_matrix_t g_vm;
std::vector<entity_t> ents;

D3DXVECTOR4 ypr_to_quat(float yaw, float pitch, float roll) {
	float cy = cos(yaw * 0.5);
	float sy = sin(yaw * 0.5);
	float cr = cos(roll * 0.5);
	float sr = sin(roll * 0.5);
	float cp = cos(pitch * 0.5);
	float sp = sin(pitch * 0.5);
	return D3DXVECTOR4(cy * sr * cp - sy * cr * sp, cy * cr * sp + sy * sr * cp, sy * cr * cp - cy * sr * sp, cy * cr * cp + sy * sr * sp);
}

D3DXVECTOR3 calc_angle(D3DXVECTOR3 pos) {
	auto dir = pos - g_vm.ViewTranslation;
	auto x = asin(dir.z / sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z)) * RAD_TO_DEG;
	auto z = atan(dir.y / dir.x) * RAD_TO_DEG;

	if (dir.x >= 0.f) z += 180.f;
	if (x > 180.0f) x -= 360.f;
	else if (x < -180.0f) x += 360.f;

	return D3DXVECTOR3(x, 0.f, z + 90.f);
}

void set_angles(uintptr_t local_player, D3DXVECTOR3 angles) {
	auto new_angles = ypr_to_quat(angles.z * DEG_TO_RAD, 0.f, angles.x * DEG_TO_RAD);

	auto skeleton = RPM<uintptr_t>(local_player + OFFSET_ENTITY_REF);
	auto viewAngle2 = RPM<uintptr_t>(skeleton + 0x1200);
	WPM<D3DXVECTOR4>(viewAngle2 + 0xC0, new_angles);
}

void populate_entity_vector() {
	struct entity_list_t { uintptr_t ent_ptrs[31]; } entity_list;
	std::vector<entity_t> buffer;

	while (1) {
		entity_list = RPM<entity_list_t>(g_entity_list + 1 * OFFSET_GAMEMANAGER_ENTITY);
		buffer.clear();

		for (uintptr_t ent_ptr : entity_list.ent_ptrs) {
			entity_t ent;
			ent.m_ptr = ent_ptr;
			ent.set_all(); if (ent.m_health < 1 || ent.m_health > 100) continue;

			buffer.push_back(ent);
		}
		
		ents = buffer;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}

void render() {
	g_d3dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	g_d3dev->BeginScene();

	float max = FLT_MAX;
	D3DXVECTOR3 closest;

	g_vm.update();

	for (entity_t ent : ents) {
		auto feet_position = g_vm.world_to_screen(ent.m_origin);  if (feet_position.z < 0.f) continue;
		auto head_position = g_vm.world_to_screen(ent.m_headpos); if (head_position.z < 0.f) continue;
		auto box_top = g_vm.world_to_screen(ent.m_top_origin);    if (box_top.z < 0.f)       continue;

		float box_height = feet_position.y - box_top.y;
		float box_width = box_height / 2.4;
		
		draw_outlined_rect(box_top.x - box_width / 2, box_top.y, box_width, box_height, epic_blue);
		draw_healthbars(box_top.x - box_width / 2 - 6, box_top.y, 2, box_height, ent.m_health, 100, epic_blue);
		draw_circle(head_position.x, head_position.y, box_height / 12.5f, 60, epic_blue);

		{
			auto tmp = head_position - D3DXVECTOR3(g_screen_width / 2, g_screen_height / 2, 0);
			float pythag = sqrt(tmp.x * tmp.x + tmp.y * tmp.y);

			if (pythag < max) {
				max = pythag;
				closest = ent.m_headpos;
			}
		}
	}

	auto closest_head = g_vm.world_to_screen(closest);
	
	if (closest_head.z > 0.f) {
		draw_line(g_screen_width / 2, g_screen_height / 2, closest_head.x, closest_head.y, epic_blue);
		if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) { set_angles(RPM<uintptr_t>(g_entity_list), calc_angle(closest)); }
	}

	g_d3dev->EndScene();
	g_d3dev->Present(NULL, NULL, NULL, NULL);
}

void pause(const char* msg) {
	std::cout << msg << "\nPress enter to continue . . ."; getchar();
	exit(EXIT_FAILURE);
}

int main() {
	SetConsoleTitleA("jizzware R6 v2");

	g_game_hwnd = FindWindowA(NULL, "Rainbow Six");

	if (g_game_hwnd) {
		std::cout << "[+] Attached to process" << std::endl;

		GetWindowThreadProcessId(g_game_hwnd, &g_process_id);
		g_process_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_process_id);

		if (g_process_handle == INVALID_HANDLE_VALUE || g_process_handle == NULL) { pause("[+] Could not attach to process"); }

		g_module = get_module("RainbowSix.exe", g_process_id);

		setup_window();
		std::cout << "[+] Created overlay" << std::endl;

		g_game_man_offset = find_pattern(g_module, "\x48\x8B\x05\x00\x00\x00\x00\x8B\x8E", "xxx????xx") + 0x3;
		g_game_man_offset += (uint64_t)RPM<uint32_t>(g_game_man_offset) + 0x4 - (uintptr_t)g_module.modBaseAddr;
		printf("[+] Found GameManager @ 0x%X\n", g_game_man_offset);

		g_prof_man_offset = find_pattern(g_module, "\x48\x8B\x05\x00\x00\x00\x00\x33\xD2\x4C\x8B\x40\x78", "xxx????xxxxxx") + 0x3;
		g_prof_man_offset += (uint64_t)RPM<uint32_t>(g_prof_man_offset) + 0x4 - (uintptr_t)g_module.modBaseAddr;
		printf("[+] Found ProfileManager @ 0x%X\n", g_prof_man_offset);

		resolve_pointers();

		std::cout << "[+] Started reading thread, starting rendering" << std::endl;
		std::thread populate_entity_vector_thread(populate_entity_vector);
		loop();
	} else { pause("[+] Game must be running to continue"); }
}
