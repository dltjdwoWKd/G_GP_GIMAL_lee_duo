#pragma comment(linker, "/entry:WinMainCRTStartup /subsystem:windows") // 콘솔창 완전히 제거
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <vector>
#include <chrono>
#include <string>
#include <random>
#include <cwchar> // 문자열 포맷팅용

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// ================================================================================
// 전역 게임 상태 관리
// ================================================================================
bool g_isGameOver = false;
float g_surviveTime = 0.0f;

// ================================================================================
// [Level 1: Foundation] 데이터 규격 및 타이머
// ================================================================================
struct Vertex { XMFLOAT3 pos; XMFLOAT4 col; };
struct ConstantBuffer { XMMATRIX matWorld; };
struct ColorBuffer { XMFLOAT4 tintColor; };

struct ShaderSet {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;

    void Release() {
        if (vs) { vs->Release(); vs = nullptr; }
        if (ps) { ps->Release(); ps = nullptr; }
        if (layout) { layout->Release(); layout = nullptr; }
    }
};

class DeltaTime {
    std::chrono::high_resolution_clock::time_point prevTime;
public:
    DeltaTime() { prevTime = std::chrono::high_resolution_clock::now(); }
    float GetDelta() {
        auto currTime = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(currTime - prevTime).count();
        prevTime = currTime;
        return dt;
    }
};

// ================================================================================
// [Level 2: Core Context] 시스템 자원 관리
// ================================================================================
class WindowContext {
public:
    HWND hWnd = nullptr;
    int Width = 1200;
    int Height = 800;

    ~WindowContext() { UnregisterClass(L"DX11EngineClass", GetModuleHandle(NULL)); }

    bool Initialize(HINSTANCE hInst, int w, int h, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM)) {
        Width = w; Height = h;
        WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, wndProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"DX11EngineClass", NULL };
        if (!RegisterClassEx(&wc)) return false;

        RECT rc = { 0, 0, w, h };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        hWnd = CreateWindow(L"DX11EngineClass", L"DX11 Runner Game (Timer & Retry)", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);

        if (!hWnd) return false;
        ShowWindow(hWnd, SW_SHOW);
        return true;
    }
};

class GraphicsContext {
public:
    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* ImmediateContext = nullptr;
    IDXGISwapChain* SwapChain = nullptr;
    ID3D11RenderTargetView* RTV = nullptr;
    int VSync = 1;
    bool IsFullscreen = false;
    int ScreenW = 1200;
    int ScreenH = 800;

    ~GraphicsContext() {
        if (SwapChain) SwapChain->SetFullscreenState(FALSE, NULL);
        if (RTV) RTV->Release();
        if (SwapChain) SwapChain->Release();
        if (ImmediateContext) ImmediateContext->Release();
        if (Device) Device->Release();
    }

    bool InitDX(HWND hWnd, int w, int h) {
        ScreenW = w; ScreenH = h;
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;

        // [중요 수정] GDI 텍스트 출력을 위해 B8G8R8A8 포맷과 GDI_COMPATIBLE 플래그 사용
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &sd, &SwapChain, &Device, NULL, &ImmediateContext);

        return SUCCEEDED(hr) && CreateRTV(w, h);
    }

    bool CreateRTV(int w, int h) {
        if (RTV) RTV->Release();
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (SUCCEEDED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer))) {
            Device->CreateRenderTargetView(pBackBuffer, NULL, &RTV);
            pBackBuffer->Release();
        }
        return true;
    }

    void SetFullscreen(bool goFull) {
        if (!SwapChain) return;
        IsFullscreen = goFull;
        SwapChain->SetFullscreenState(goFull, NULL);
    }

    // [신규] DX11 스왑체인 위에 GDI를 이용해 텍스트를 그리는 함수
    void DrawTextOnScreen(const std::wstring& text, int x, int y, int fontSize, COLORREF color, bool center = false) {
        IDXGISurface1* pSurface = nullptr;
        if (SUCCEEDED(SwapChain->GetBuffer(0, __uuidof(IDXGISurface1), (void**)&pSurface))) {
            HDC hdc;
            if (SUCCEEDED(pSurface->GetDC(FALSE, &hdc))) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, color);
                HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
                HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

                if (center) {
                    RECT rect = { 0, y, ScreenW, y + fontSize + 20 };
                    DrawTextW(hdc, text.c_str(), -1, &rect, DT_CENTER | DT_NOCLIP);
                }
                else {
                    TextOutW(hdc, x, y, text.c_str(), (int)text.length());
                }

                SelectObject(hdc, hOldFont);
                DeleteObject(hFont);
                pSurface->ReleaseDC(nullptr);
            }
            pSurface->Release();
        }
    }

    ShaderSet CompileShaderMemory(const std::string& src, D3D11_INPUT_ELEMENT_DESC* ied, UINT iedCount) {
        ShaderSet res;
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, nullptr);
        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, nullptr);
        if (vsBlob) {
            Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &res.vs);
            Device->CreateInputLayout(ied, iedCount, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &res.layout);
            vsBlob->Release();
        }
        if (psBlob) {
            Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &res.ps);
            psBlob->Release();
        }
        return res;
    }
};

// ================================================================================
// [Level 3 & 4: Resources & Scene System] 컴포넌트 구조
// ================================================================================
class GameObject;
class Component {
public:
    GameObject* pOwner = nullptr;
    bool isStarted = false;
    virtual void Start(GraphicsContext* gfx) = 0;
    virtual void Input() = 0;
    virtual void Update(float dt) = 0;
    virtual void Render(GraphicsContext* gfx) = 0;
    virtual void Reset() {} // [신규] 재시작 시 호출될 초기화 인터페이스
    virtual ~Component() {}
};

class GameObject {
public:
    XMFLOAT3 pos = { 0, 0, 0 };
    XMFLOAT3 rot = { 0, 0, 0 };
    XMFLOAT3 scale = { 1, 1, 1 };
    std::vector<Component*> components;

    GameObject(float x, float y, float z) { pos = { x, y, z }; }
    ~GameObject() { for (auto c : components) delete c; }

    void AddComponent(Component* c) { c->pOwner = this; components.push_back(c); }
    void Input() { for (auto c : components) c->Input(); }
    void Update(float dt, GraphicsContext* gfx) {
        for (auto c : components) {
            if (!c->isStarted) { c->Start(gfx); c->isStarted = true; }
            c->Update(dt);
        }
    }
    void Render(GraphicsContext* gfx) { for (auto c : components) c->Render(gfx); }
    void Reset() { for (auto c : components) c->Reset(); } // [신규] 자식 컴포넌트 일괄 리셋
};

// ... (Mesh, Material, ColorMaterial, MeshRenderer 코드는 이전과 동일) ...
class Mesh {
public:
    ID3D11Buffer* vBuffer = nullptr;
    UINT vertexCount = 0;
    ~Mesh() { if (vBuffer) vBuffer->Release(); }
    void Create(GraphicsContext* gfx, const std::vector<Vertex>& vertices) {
        vertexCount = (UINT)vertices.size();
        D3D11_BUFFER_DESC bd = { 0 }; bd.Usage = D3D11_USAGE_DEFAULT; bd.ByteWidth = sizeof(Vertex) * vertexCount; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { 0 }; sd.pSysMem = vertices.data();
        gfx->Device->CreateBuffer(&bd, &sd, &vBuffer);
    }
};
class Material {
public:
    ShaderSet* shaders = nullptr;
    Material(ShaderSet* s) : shaders(s) {}
    virtual ~Material() {}
    virtual void Bind(ID3D11DeviceContext* context) = 0;
};
class ColorMaterial : public Material {
public:
    XMFLOAT4 color; ID3D11Buffer* pColorBuffer = nullptr;
    ColorMaterial(ShaderSet* s, XMFLOAT4 col, ID3D11Device* device) : Material(s), color(col) {
        D3D11_BUFFER_DESC cbd = { 0 }; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.ByteWidth = sizeof(ColorBuffer); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&cbd, nullptr, &pColorBuffer);
    }
    ~ColorMaterial() { if (pColorBuffer) pColorBuffer->Release(); }
    void Bind(ID3D11DeviceContext* context) override {
        context->IASetInputLayout(shaders->layout); context->VSSetShader(shaders->vs, nullptr, 0); context->PSSetShader(shaders->ps, nullptr, 0);
        ColorBuffer cb = { color }; context->UpdateSubresource(pColorBuffer, 0, nullptr, &cb, 0, 0); context->PSSetConstantBuffers(1, 1, &pColorBuffer);
    }
};
class MeshRenderer : public Component {
public:
    Mesh* pMeshData = nullptr; Material* pMaterial = nullptr; ID3D11Buffer* cBuffer = nullptr;
    MeshRenderer(Mesh* mesh, Material* mat) : pMeshData(mesh), pMaterial(mat) {}
    ~MeshRenderer() { if (cBuffer) cBuffer->Release(); }
    void Start(GraphicsContext* gfx) override {
        D3D11_BUFFER_DESC cbd = { 0 }; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.ByteWidth = sizeof(ConstantBuffer); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        gfx->Device->CreateBuffer(&cbd, nullptr, &cBuffer);
    }
    void Input() override {} void Update(float dt) override {}
    void Render(GraphicsContext* gfx) override {
        if (!pMeshData || !pMaterial) return;
        pMaterial->Bind(gfx->ImmediateContext);
        XMMATRIX world = XMMatrixScaling(pOwner->scale.x, pOwner->scale.y, 1.0f) * XMMatrixRotationZ(pOwner->rot.z) * XMMatrixTranslation(pOwner->pos.x, pOwner->pos.y, 0.0f);
        ConstantBuffer cb; cb.matWorld = XMMatrixTranspose(world);
        gfx->ImmediateContext->UpdateSubresource(cBuffer, 0, nullptr, &cb, 0, 0);
        gfx->ImmediateContext->VSSetConstantBuffers(0, 1, &cBuffer);
        UINT stride = sizeof(Vertex), offset = 0;
        gfx->ImmediateContext->IASetVertexBuffers(0, 1, &pMeshData->vBuffer, &stride, &offset);
        gfx->ImmediateContext->Draw(pMeshData->vertexCount, 0);
    }
};

// ================================================================================
// 1. 러닝 액션: 플레이어 제어 컴포넌트
// ================================================================================
class RunnerPlayerController : public Component
{
    float velocityY = 0.0f;
    float gravity = -18.0f;
    float jumpPower = 5.5f;
    float groundY = -0.6f;

    float originalScaleY = 0.0f;
    bool isGrounded = false;
    bool isSliding = false;
    int jumpCount = 0;
    bool prevUpPressed = false;

public:
    void Start(GraphicsContext* gfx) override {
        originalScaleY = pOwner->scale.y;
        Reset(); // 초기화 로직 재사용
    }

    // [신규] 재시작 시 플레이어를 바닥으로 복귀시키는 기능
    void Reset() override {
        pOwner->pos.y = groundY + (originalScaleY * 0.5f);
        pOwner->scale.y = originalScaleY;
        velocityY = 0.0f;
        isGrounded = false;
        isSliding = false;
        jumpCount = 0;
    }

    void Input() override {
        bool isUpPressed = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        if (isUpPressed && !prevUpPressed && !isSliding) {
            if (isGrounded) { velocityY = jumpPower; isGrounded = false; jumpCount = 1; }
            else if (jumpCount == 1) { velocityY = jumpPower * 0.9f; jumpCount = 2; }
        }
        prevUpPressed = isUpPressed;

        if ((GetAsyncKeyState(VK_DOWN) & 0x8000)) {
            isSliding = true;
            if (!isGrounded) velocityY -= 1.0f;
        }
        else {
            isSliding = false;
        }
    }

    void Update(float dt) override {
        if (!isGrounded) {
            velocityY += gravity * dt;
            pOwner->pos.y += velocityY * dt;
        }

        float currentHeight = isSliding ? (originalScaleY * 0.5f) : originalScaleY;
        pOwner->scale.y = currentHeight;

        float bottomY = pOwner->pos.y - (currentHeight * 0.5f);
        if (bottomY <= groundY) {
            pOwner->pos.y = groundY + (currentHeight * 0.5f);
            velocityY = 0.0f;
            isGrounded = true;
            jumpCount = 0;
        }
        else {
            isGrounded = false;
        }
    }
    void Render(GraphicsContext* gfx) override {}
};

// ================================================================================
// 2. 장애물 컨트롤러 (충돌 시 g_isGameOver 발동)
// ================================================================================
class ObstacleController : public Component
{
    GameObject* player;
    float speed = 1.3f;
    float groundY = -0.6f;
    std::mt19937 gen;
    std::uniform_int_distribution<int> typeDist;

public:
    bool isActive = false;
    ObstacleController(GameObject* pTarget) : player(pTarget), typeDist(0, 3) {
        std::random_device rd; gen = std::mt19937(rd());
    }

    void Start(GraphicsContext* gfx) override { Reset(); }

    // [신규] 장애물을 화면 밖으로 치우는 초기화 기능
    void Reset() override {
        isActive = false;
        pOwner->pos.x = -10.0f;
    }

    void Input() override {}
    void Update(float dt) override {
        if (!isActive) return;
        pOwner->pos.x -= speed * dt;
        if (pOwner->pos.x < -1.5f) { Reset(); } // 화면 벗어나면 대기실로

        float px = player->pos.x; float py = player->pos.y;
        float pw = player->scale.x; float ph = player->scale.y;
        float ox = pOwner->pos.x; float oy = pOwner->pos.y;
        float ow = pOwner->scale.x; float oh = pOwner->scale.y;

        // 충돌 체크
        if (abs(px - ox) < (pw + ow) * 0.45f && abs(py - oy) < (ph + oh) * 0.45f) {
            g_isGameOver = true; // [중요 수정] 충돌 시 전역 사망 플래그 ON!
        }
    }
    void Render(GraphicsContext* gfx) override {}

    void Spawn() {
        isActive = true; pOwner->pos.x = 1.5f;
        std::uniform_real_distribution<float> speedDist(1.2f, 1.6f);
        speed = speedDist(gen);
        switch (typeDist(gen)) {
        case 0: pOwner->scale = { 0.15f, 0.2f, 1.0f }; pOwner->pos.y = groundY + (pOwner->scale.y * 0.5f); break;
        case 1: pOwner->scale = { 0.2f, 0.45f, 1.0f }; pOwner->pos.y = groundY + (pOwner->scale.y * 0.5f); break;
        case 2: pOwner->scale = { 0.15f, 0.3f, 1.0f }; pOwner->pos.y = groundY + 0.45f; break;
        case 3: pOwner->scale = { 0.7f, 0.3f, 1.0f }; pOwner->pos.y = groundY + 0.45f; break;
        }
    }
};

// ================================================================================
// 3. 게임 난이도 매니저 (시간 리셋 기능 포함)
// ================================================================================
class ObstacleManager : public Component {
    std::vector<ObstacleController*> pool;
    float totalTime = 0.0f;
    float spawnTimer = 5.0f;

public:
    void AddObstacleToPool(ObstacleController* obs) { pool.push_back(obs); }
    void Start(GraphicsContext* gfx) override {}

    // [신규] 난이도 시간 초기화 기능
    void Reset() override {
        totalTime = 0.0f;
        spawnTimer = 5.0f;
    }

    void Input() override {}
    void Update(float dt) override {
        totalTime += dt;
        spawnTimer += dt;

        float currentInterval = 5.0f;
        if (totalTime >= 120.0f) currentInterval = 2.0f;
        else if (totalTime >= 90.0f) currentInterval = 3.0f;
        else if (totalTime >= 60.0f) currentInterval = 3.5f;
        else if (totalTime >= 30.0f) currentInterval = 4.0f;

        if (spawnTimer >= currentInterval) {
            spawnTimer = 0.0f;
            for (auto obs : pool) {
                if (!obs->isActive) { obs->Spawn(); break; }
            }
        }
    }
    void Render(GraphicsContext* gfx) override {}
};

// ================================================================================
// [Level 5: Entry] 게임 루프 사령탑
// ================================================================================
class GameLoop {
public:
    WindowContext win;
    GraphicsContext gfx;
    DeltaTime timer;
    std::vector<GameObject*> world;
    bool isRunning = true;

    ~GameLoop() {
        for (auto obj : world) delete obj;
        world.clear();
    }

    void Initialize(HINSTANCE hInst, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM)) {
        win.Initialize(hInst, 1200, 800, wndProc);
        gfx.InitDX(win.hWnd, 1200, 800);
    }

    void Input() {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) isRunning = false;
        if (GetAsyncKeyState('F') & 0x0001) gfx.SetFullscreen(!gfx.IsFullscreen);

        // 사망 상태에서 R키를 누르면 게임 재시작!
        if (g_isGameOver && (GetAsyncKeyState('R') & 0x8000)) {
            g_isGameOver = false;
            g_surviveTime = 0.0f;
            for (auto obj : world) obj->Reset();
            timer.GetDelta(); // 일시정지 되어있던 시간 오차를 한 번 비워냄
        }

        if (!g_isGameOver) {
            for (auto obj : world) obj->Input();
        }
    }

    void Update() {
        if (!g_isGameOver) {
            float dt = timer.GetDelta();
            g_surviveTime += dt; // 살아남은 시간 누적
            for (auto obj : world) obj->Update(dt, &gfx);
        }
    }

    void Render() {
        float clearColor[] = { 0.15f, 0.15f, 0.15f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, clearColor);

        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)win.Width, (float)win.Height, 0.0f, 1.0f };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 1. 3D 오브젝트 먼저 렌더링
        for (auto obj : world) obj->Render(&gfx);

        // 2. [신규] UI 텍스트 렌더링 (3D 위에 덮어쓰기)
        wchar_t timeText[64];
        swprintf_s(timeText, L"Time: %.2f s", g_surviveTime);
        gfx.DrawTextOnScreen(timeText, 30, 30, 36, RGB(255, 255, 255)); // 좌상단 시간 출력

        // 죽었을 때 출력할 텍스트
        if (g_isGameOver) {
            gfx.DrawTextOnScreen(L"YOU DIED!", 0, 300, 100, RGB(255, 50, 50), true);
            gfx.DrawTextOnScreen(L"re try? press r", 0, 420, 40, RGB(200, 200, 200), true);
        }

        gfx.SwapChain->Present(gfx.VSync, 0);
    }

    void Run() {
        MSG msg = {};
        while (msg.message != WM_QUIT && isRunning) {
            if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            else {
                Input();
                Update();
                Render();
            }
        }
    }
};

LRESULT CALLBACK GlobalWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

// ================================================================================
// 메인 엔트리
// ================================================================================
int WINAPI WinMain(_In_ HINSTANCE hI, _In_opt_ HINSTANCE hPrev, _In_ LPSTR pCmd, _In_ int nShow) {
    // 콘솔창 생성을 지우고 온전한 게임 클라이언트로 구동됩니다.
    GameLoop gEngine;
    gEngine.Initialize(hI, GlobalWndProc);

    std::string shaderSrc = R"(
        cbuffer cbWorld : register(b0) { matrix matWorld; };
        cbuffer cbMaterial : register(b1) { float4 tintColor; };
        struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };
        struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };
        
        PS_IN VS(VS_IN input) {
            PS_IN output;
            output.pos = mul(float4(input.pos, 1.0f), matWorld);
            output.col = input.col;
            return output;
        }
        float4 PS(PS_IN input) : SV_Target { return tintColor; }
    )";

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ShaderSet baseShaders = gEngine.gfx.CompileShaderMemory(shaderSrc, ied, 2);

    std::vector<Vertex> vQuad = {
        { {-0.5f,  0.5f, 0.0f}, {1,1,1,1} }, { { 0.5f,  0.5f, 0.0f}, {1,1,1,1} }, { { 0.5f, -0.5f, 0.0f}, {1,1,1,1} },
        { {-0.5f,  0.5f, 0.0f}, {1,1,1,1} }, { { 0.5f, -0.5f, 0.0f}, {1,1,1,1} }, { {-0.5f, -0.5f, 0.0f}, {1,1,1,1} }
    };
    Mesh* quadMesh = new Mesh();
    quadMesh->Create(&gEngine.gfx, vQuad);

    ColorMaterial* playerMat = new ColorMaterial(&baseShaders, { 0.2f, 0.6f, 1.0f, 1.0f }, gEngine.gfx.Device);
    ColorMaterial* groundMat = new ColorMaterial(&baseShaders, { 0.4f, 0.4f, 0.4f, 1.0f }, gEngine.gfx.Device);
    ColorMaterial* obstacleMat = new ColorMaterial(&baseShaders, { 1.0f, 0.2f, 0.2f, 1.0f }, gEngine.gfx.Device);

    // [바닥]
    GameObject* ground = new GameObject(0.0f, -0.8f, 0.0f);
    ground->scale = { 3.0f, 0.4f, 1.0f };
    ground->AddComponent(new MeshRenderer(quadMesh, groundMat));
    gEngine.world.push_back(ground);

    // [플레이어]
    GameObject* player = new GameObject(-0.5f, 0.0f, 0.0f);
    player->scale = { 0.25f, 0.25f, 1.0f };
    player->AddComponent(new MeshRenderer(quadMesh, playerMat));
    player->AddComponent(new RunnerPlayerController());
    gEngine.world.push_back(player);

    // [스포너]
    GameObject* spawnerObj = new GameObject(0.0f, 0.0f, 0.0f);
    ObstacleManager* spawner = new ObstacleManager();
    spawnerObj->AddComponent(spawner);
    gEngine.world.push_back(spawnerObj);

    // [장애물 풀]
    for (int i = 0; i < 4; i++) {
        GameObject* obs = new GameObject(-10.0f, 0.0f, 0.0f);
        obs->AddComponent(new MeshRenderer(quadMesh, obstacleMat));
        ObstacleController* controller = new ObstacleController(player);
        obs->AddComponent(controller);
        spawner->AddObstacleToPool(controller);
        gEngine.world.push_back(obs);
    }

    gEngine.Run();

    delete playerMat;
    delete groundMat;
    delete obstacleMat;
    baseShaders.Release();
    delete quadMesh;

    return 0;
}