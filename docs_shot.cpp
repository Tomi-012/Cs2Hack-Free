// Render menu Cs2Hack-Free ke texture offscreen D3D11 WARP (docs).
// Tanpa attach game, tanpa tulis config.
#include "cheat/overlay.cpp"
#include "third_party/imgui/imgui_internal.h"
#include <vector>
#include <cstring>
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static void capture(const char* filename, ID3D11Texture2D* texture, int w, int h) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* copy = nullptr;
    dev->CreateTexture2D(&desc, nullptr, &copy);
    ctx->CopyResource(copy, texture);
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(copy, 0, D3D11_MAP_READ, 0, &map);
    BITMAPFILEHEADER fh{}; fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    BITMAPINFOHEADER bi{}; bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32;
    FILE* f = fopen(filename, "wb");
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f);
    std::vector<unsigned char> row(w * 4);
    for (int y = 0; y < h; ++y) {
        auto* p = (unsigned char*)map.pData + y * map.RowPitch;
        for (int x = 0; x < w; ++x) {
            row[x*4] = p[x*4+2]; row[x*4+1] = p[x*4+1]; row[x*4+2] = p[x*4]; row[x*4+3] = 255;
        }
        fwrite(row.data(), row.size(), 1, f);
    }
    fclose(f); ctx->Unmap(copy, 0); copy->Release();
}
static void scenario(const char* name, const char* tab) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = {800, 700}; io.DeltaTime = 1.f / 60.f;
    ImGui_ImplWin32_Init(Overlay::g_hwnd);
    ImGui_ImplDX11_Init(dev, ctx);
    Overlay::g_menu_open = true;
    Overlay::g_screen_width = 800; Overlay::g_screen_height = 700;
    Overlay::s_docs_mode = true;
    Overlay::g_pd3dDevice = dev; Overlay::g_pd3dDeviceContext = ctx;
    std::array<PlayerData, k_max_entities> players{};
    for (int i = 1; i <= 10; ++i) {
        auto& p = players[i];
        p.pawn_ptr = 0x1000 + i; p.ctrl_ptr = 0x2000 + i; p.controller_index = i;
        p.health = 100 - i * 5; p.armor = 50; p.team = (i % 2) ? 2 : 3;
        p.weapon_def = 7; p.clip = 25; p.clip_capacity = 30;
        p.distance = 12.5f * i; p.alive = true;
        snprintf(p.name, sizeof p.name, "Player%d", i);
        p.origin = {100.f * i, 200.f, 0.f};
        p.head_pos = {100.f * i, 200.f, 72.f};
    }
    Mat4x4 vm{};
    vm.m[0][0] = vm.m[1][1] = vm.m[2][2] = vm.m[3][3] = 1.f;
    BombInfo bomb{};
    HitState hit{};
    ID3D11Texture2D* texture = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 800; d.Height = 700; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&d, nullptr, &texture);
    dev->CreateRenderTargetView(texture, nullptr, &rtv);
    Overlay::g_mainRenderTargetView = rtv;
    for (int frame = 0; frame < 5; ++frame) {
        if (frame > 0) {
            auto& g = *ImGui::GetCurrentContext();
            for (int j = 0; j < g.TabBars.GetMapSize(); ++j) {
                auto* bar = g.TabBars.TryGetMapData(j);
                if (!bar) continue;
                for (auto& tb : bar->Tabs)
                    if (!strcmp(ImGui::TabBarGetTabName(bar, &tb), tab))
                        bar->SelectedTabId = bar->NextSelectedTabId = tb.ID;
            }
        }
        // RenderFrame penuh (NewFrame di dalam; docs_mode: berhenti setelah ImGui::Render)
        Overlay::RenderFrame(players, 3, true, vm, Vec3{}, 0.f, false,
                             true, bomb, hit, 14181, 490.0, GetTickCount());
        float bg[] = {.035f,.045f,.06f,1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    char path[256]; std::snprintf(path, sizeof path, "%s.bmp", name);
    capture(path, texture, 800, 700);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release(); texture->Release();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    std::printf("%s ok\n", name);
}
static LRESULT WINAPI DummyWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
int main() {
    D3D_FEATURE_LEVEL lv = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, &lv, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { std::printf("D3D failure %lx\n", (unsigned long)hr); return 1; }
    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc; wc.lpfnWndProc = DummyWnd;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Cs2FreeDocs";
    RegisterClassExW(&wc);
    Overlay::g_hwnd = CreateWindowExW(0, L"Cs2FreeDocs", L"docs",
        WS_POPUP, 0, 0, 800, 700, nullptr, nullptr, wc.hInstance, nullptr);
    g_game_build = 14181;
    scenario("ui-esp", " ESP ");
    scenario("ui-radar", " Radar ");
    scenario("ui-misc", " Misc ");
    scenario("ui-players", " Players ");
    scenario("ui-status", " Status ");
    scenario("ui-presets", " Presets ");
    DestroyWindow(Overlay::g_hwnd);
    ctx->Release(); dev->Release();
    return 0;
}
