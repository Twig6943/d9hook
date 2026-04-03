#include <Windows.h>
#include <string>

// disable unfixable warnings (you might need to uncomment this if needed)
#pragma warning(push, 0)
#include <d3d9.h>
#include <d3dx9.h>
#include <detours.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#pragma warning(pop)

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#pragma comment(lib, "detours.lib")

typedef HRESULT(_stdcall* EndScene)(LPDIRECT3DDEVICE9 pDevice);
HRESULT _stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice);

#include <dinput.h>

extern "C" __declspec(dllexport)
HRESULT __stdcall proxyDirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter)
{
    using tDirectInput8Create = HRESULT(__stdcall*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

    static HMODULE realDinput8 = nullptr;
    static tDirectInput8Create pDirectInput8Create = nullptr;

    if (!realDinput8)
    {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\dinput8.dll");

        realDinput8 = LoadLibraryA(sysPath);
    }

    if (!realDinput8)
        return DIERR_GENERIC;

    if (!pDirectInput8Create)
    {
        pDirectInput8Create = reinterpret_cast<tDirectInput8Create>(
            GetProcAddress(realDinput8, "DirectInput8Create")
        );
    }

    if (!pDirectInput8Create)
        return DIERR_GENERIC;

    return pDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

namespace Globals
{
    static LPDIRECT3DDEVICE9 PD3D_DEVICE = nullptr;
    static IDirect3D9* PD3D = nullptr;
    static HWND    WINDOW = nullptr;
    static WNDPROC WNDPROC_ORIGNAL = nullptr;
    static bool isMenuToggled = false;
    static bool isInit = false;
    static EndScene oEndScene;
}

// -------- UTILS ----------
BOOL CALLBACK EnumWidnowsCallback(HWND handle, LPARAM lParam)
{
    DWORD wndProcID;
    GetWindowThreadProcessId(handle, &wndProcID);

    if (GetCurrentProcessId() != wndProcID)
    {
        return true;
    }

    Globals::WINDOW = handle;
    return false;
}

HWND GetProcessWindow()
{
    EnumWindows(EnumWidnowsCallback, NULL);
    return Globals::WINDOW;
}

// Return true when toggled to redirect the input to the ui instead of the game
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (Globals::isMenuToggled && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    {
        return true;
    }

    return CallWindowProc(Globals::WNDPROC_ORIGNAL, hWnd, msg, wParam, lParam);
}

void DrawMenu()
{
    // TO DO: Create your cheat window here or put function to create it here !
    ImGui::Begin("Faces Menu", &Globals::isMenuToggled);
    static bool isChamsToggled = false;
    ImGui::Checkbox("Chams", &isChamsToggled);
    // --------
    ImGui::End();
}

// -------- DIRECTX9 -------
bool GetD3D9Device(void** pTable, size_t size)
{
    if (!pTable)
        return false;

    // a
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "D3D9HookDummy";
    RegisterClassExA(&wc);

    HWND dummyWindow = CreateWindowExA(
        0, "D3D9HookDummy", "D3D9Hook",
        WS_OVERLAPPED, 0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    if (!dummyWindow)
    {
        UnregisterClassA("D3D9HookDummy", GetModuleHandleA(nullptr));
        return false;
    }

    // Create a D3D Variable and get the sdk version
    Globals::PD3D = Direct3DCreate9(D3D_SDK_VERSION);

    // Make sure that the pointer is valid
    if (!Globals::PD3D)
    {
        DestroyWindow(dummyWindow);
        UnregisterClassA("D3D9HookDummy", GetModuleHandleA(nullptr));
        return false;
    }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.SwapEffect    = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = dummyWindow;
    d3dpp.Windowed      = TRUE;

    // Use software vertex processing for the dummy device — it only exists to
    // steal the vtable and hardware VP can fail in constrained Vulkan contexts.
    Globals::PD3D->CreateDevice(D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        dummyWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &d3dpp,
        &Globals::PD3D_DEVICE);

    if (!Globals::PD3D_DEVICE)
    {
        Globals::PD3D->Release();
        DestroyWindow(dummyWindow);
        UnregisterClassA("D3D9HookDummy", GetModuleHandleA(nullptr));
        return false;
    }

    // We are copying the pTable that we get from the device and its gonna be the size of the pTable
    memcpy(pTable, *reinterpret_cast<void***>(Globals::PD3D_DEVICE), size);

    // Release everything at the end
    Globals::PD3D_DEVICE->Release();
    Globals::PD3D->Release();
    DestroyWindow(dummyWindow);
    UnregisterClassA("D3D9HookDummy", GetModuleHandleA(nullptr));

    return true;
}

void CleanUpDeviceD3D()
{
    if (Globals::PD3D_DEVICE)
    {
        Globals::PD3D_DEVICE->Release();
        Globals::PD3D_DEVICE = nullptr;
    }

    if (Globals::PD3D)
    {
        Globals::PD3D->Release();
        Globals::PD3D = nullptr;
    }
}

// -------- HOOK ----------
HRESULT __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
    if (!Globals::isInit)
    {
        // Call the original game message handling fnc
        Globals::WNDPROC_ORIGNAL = (WNDPROC)SetWindowLongPtr(Globals::WINDOW, GWLP_WNDPROC, (LONG_PTR)WndProc);

        // Draw the ImGui Menu
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(Globals::WINDOW);
        ImGui_ImplDX9_Init(pDevice);

        // Init to true to prevent spamming the message box
        Globals::isInit = true;
    }

    if (GetAsyncKeyState(VK_INSERT) & 1)
    {
        Globals::isMenuToggled = !Globals::isMenuToggled;
        Sleep(1);
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (Globals::isMenuToggled)
        DrawMenu();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    // Hook the EndScene
    return Globals::oEndScene(pDevice);
}

DWORD WINAPI InitHook(PVOID base)
{
	// Sleep for a while
    while (!GetProcessWindow())
        Sleep(100);

    // store a VTable of 119 functions
    void* d3d9Device[119];

    if (GetD3D9Device(d3d9Device, sizeof(d3d9Device)))
    {
        // Hook the endScene
        Globals::oEndScene = (EndScene)d3d9Device[42];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Globals::oEndScene, hkEndScene);
        DetourTransactionCommit();


        while (!GetAsyncKeyState(VK_END))
        {
            Sleep(1);
        }

        // Unhook the endScene
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)Globals::oEndScene, hkEndScene);
        DetourTransactionCommit();

        CleanUpDeviceD3D();
    }

    FreeLibraryAndExitThread(static_cast<HMODULE>(base), 1);
}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
       CreateThread(nullptr, NULL, InitHook, hModule, NULL, nullptr);
       break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}
