// Measures the cost of the vertex range passed to IDirect3DDevice9::DrawIndexedPrimitive.
//
// Dear ImGui's DX9 backend declares
//     MinIndex = 0, NumVertices = <the whole draw list's vertex count>
// for every draw command. On hardware vertex processing this is only a validation bound and
// costs nothing. On a device created with D3DCREATE_SOFTWARE_VERTEXPROCESSING the declared
// range is what gets transformed, so the frame's vertex work becomes
// (draw commands) x (draw list vertices).
//
// This harness builds one ImGui frame, then submits the very same ImDrawData twice: once with
// the upstream arguments and once with the range each command's indices actually reference.
// Only those four arguments differ - the state setup, buffers, uploads and clip rects are
// identical - so any difference is attributable to the range alone.
//
// Build: build.cmd
//
// Run:
//   dx9_vtxrange_bench.exe            # software vertex processing (the interesting case)
//   dx9_vtxrange_bench.exe --hwvp     # hardware vertex processing (the control)

#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "imgui.h"

namespace
{

    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    constexpr int kMeasureFrames = 120;
    constexpr int kRepeats = 3;

    using Clock = std::chrono::steady_clock;

    double ms_since(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // Same vertex the DX9 backends use
    struct Vertex
    {
        float pos[3];
        unsigned int col;
        float uv[2];
    };

    constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    HWND g_window = nullptr;

    LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool create_window()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = L"dx9_vtxrange_bench";

        if (!RegisterClassExW(&wc))
        {
            return false;
        }

        g_window = CreateWindowExW(0, wc.lpszClassName, L"DX9 vertex range benchmark",
                                   WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, 40, 40, kWidth, kHeight,
                                   nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_window)
        {
            return false;
        }

        ShowWindow(g_window, SW_SHOWNORMAL);
        UpdateWindow(g_window);

        return true;
    }

    void pump_messages()
    {
        MSG msg;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    const char *device_type_name(D3DDEVTYPE type)
    {
        switch (type)
        {
        case D3DDEVTYPE_HAL:
            return "HAL";
        case D3DDEVTYPE_REF:
            return "reference rasterizer";
        case D3DDEVTYPE_SW:
            return "software rasterizer";
        case D3DDEVTYPE_NULLREF:
            return "null reference";
        default:
            return "unknown";
        }
    }

    // ---------------------------------------------------------------- submit path

    struct SubmitState
    {
        IDirect3DDevice9 *device = nullptr;
        IDirect3DTexture9 *font_texture = nullptr;
        IDirect3DVertexBuffer9 *vertex_buffer = nullptr;
        IDirect3DIndexBuffer9 *index_buffer = nullptr;
        UINT vertex_capacity = 0;
        UINT index_capacity = 0;
        std::vector<Vertex> vertices;
        std::vector<ImDrawIdx> indices;
    };

    void release_vertex_buffer(SubmitState &state)
    {
        if (state.vertex_buffer)
        {
            state.vertex_buffer->Release();
            state.vertex_buffer = nullptr;
        }
    }

    void release_index_buffer(SubmitState &state)
    {
        if (state.index_buffer)
        {
            state.index_buffer->Release();
            state.index_buffer = nullptr;
        }
    }

    // Upload the frame, exactly like the backends do: one dynamic vertex buffer and one dynamic
    // index buffer, both locked with D3DLOCK_DISCARD.
    void upload(SubmitState &state, const ImDrawData *draw_data)
    {
        state.vertices.clear();
        state.indices.clear();
        state.vertices.reserve(static_cast<size_t>(draw_data->TotalVtxCount) + 1);
        state.indices.reserve(static_cast<size_t>(draw_data->TotalIdxCount) + 1);

        for (const ImDrawList *list : draw_data->CmdLists)
        {
            for (const ImDrawVert &vertex : list->VtxBuffer)
            {
                Vertex out;
                out.pos[0] = vertex.pos.x;
                out.pos[1] = vertex.pos.y;
                out.pos[2] = 0.0f;
                // ImGui packs IM_COL32 as 0xAABBGGRR, D3DCOLOR wants 0xAARRGGBB.
                const unsigned int col = vertex.col;
                out.col = (col & 0xFF00FF00u) | ((col & 0x000000FFu) << 16) | ((col & 0x00FF0000u) >> 16);
                out.uv[0] = vertex.uv.x;
                out.uv[1] = vertex.uv.y;
                state.vertices.push_back(out);
            }
            state.indices.insert(state.indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        }

        if (state.vertices.size() > state.vertex_capacity)
        {
            release_vertex_buffer(state);
            state.vertex_capacity = static_cast<UINT>(state.vertices.size());
            state.device->CreateVertexBuffer(state.vertex_capacity * sizeof(Vertex),
                                             D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                             D3DPOOL_DEFAULT, &state.vertex_buffer, nullptr);
        }

        if (state.indices.size() > state.index_capacity)
        {
            release_index_buffer(state);
            state.index_capacity = static_cast<UINT>(state.indices.size());
            state.device->CreateIndexBuffer(
                state.index_capacity * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT,
                &state.index_buffer, nullptr);
        }

        void *data = nullptr;
        state.vertex_buffer->Lock(0, 0, &data, D3DLOCK_DISCARD);
        std::memcpy(data, state.vertices.data(), state.vertices.size() * sizeof(Vertex));
        state.vertex_buffer->Unlock();

        state.index_buffer->Lock(0, 0, &data, D3DLOCK_DISCARD);
        std::memcpy(data, state.indices.data(), state.indices.size() * sizeof(ImDrawIdx));
        state.index_buffer->Unlock();
    }

    void setup_render_state(SubmitState &state, const ImDrawData *draw_data)
    {
        IDirect3DDevice9 *device = state.device;

        D3DVIEWPORT9 viewport = {0, 0, static_cast<DWORD>(draw_data->DisplaySize.x),
                                 static_cast<DWORD>(draw_data->DisplaySize.y), 0.0f, 1.0f};
        device->SetViewport(&viewport);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetFVF(kFvf);
        device->SetStreamSource(0, state.vertex_buffer, 0, sizeof(Vertex));
        device->SetIndices(state.index_buffer);

        // Orthographic projection over the display area, as the backends compute it.
        const float left = draw_data->DisplayPos.x + 0.5f;
        const float top = draw_data->DisplayPos.y + 0.5f;
        const float right = draw_data->DisplayPos.x + draw_data->DisplaySize.x + 0.5f;
        const float bottom = draw_data->DisplayPos.y + draw_data->DisplaySize.y + 0.5f;

        D3DMATRIX identity = {};
        identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
        D3DMATRIX projection = identity;
        projection._11 = 2.0f / (right - left);
        projection._22 = 2.0f / (top - bottom);
        projection._33 = 0.5f;
        projection._41 = (right + left) / (left - right);
        projection._42 = (top + bottom) / (bottom - top);
        projection._43 = 0.5f;
        device->SetTransform(D3DTS_WORLD, &identity);
        device->SetTransform(D3DTS_VIEW, &identity);
        device->SetTransform(D3DTS_PROJECTION, &projection);
    }

    struct DrawCounts
    {
        unsigned int commands = 0;
        unsigned int vertices = 0;
        unsigned int declared_vertices = 0; // sum of NumVertices over all draw calls
    };

    // The only code that differs between the two variants.
    void draw(SubmitState &state, const ImDrawData *draw_data, bool exact_vertex_range,
              DrawCounts *counts)
    {
        IDirect3DDevice9 *device = state.device;
        UINT vertex_base = 0;
        UINT index_base = 0;

        for (const ImDrawList *list : draw_data->CmdLists)
        {
            const UINT list_vertices = static_cast<UINT>(list->VtxBuffer.Size);

            for (const ImDrawCmd &command : list->CmdBuffer)
            {
                if (command.UserCallback != nullptr)
                {
                    continue;
                }

                RECT rect;
                rect.left = static_cast<LONG>(command.ClipRect.x - draw_data->DisplayPos.x);
                rect.top = static_cast<LONG>(command.ClipRect.y - draw_data->DisplayPos.y);
                rect.right = static_cast<LONG>(command.ClipRect.z - draw_data->DisplayPos.x);
                rect.bottom = static_cast<LONG>(command.ClipRect.w - draw_data->DisplayPos.y);

                if (rect.right <= rect.left || rect.bottom <= rect.top)
                {
                    continue;
                }

                device->SetScissorRect(&rect);
                device->SetTexture(0, state.font_texture);

                if (!exact_vertex_range)
                {
                    // Upstream: the whole draw list, every command.
                    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                                 command.VtxOffset + vertex_base, 0, list_vertices,
                                                 command.IdxOffset + index_base,
                                                 command.ElemCount / 3);
                    counts->declared_vertices += list_vertices;
                }
                else
                {
                    // Patched: only what this command's indices reach.
                    unsigned int min_index = UINT_MAX;
                    unsigned int max_index = 0;
                    const ImDrawIdx *indices = &list->IdxBuffer[command.IdxOffset];
                    for (unsigned int n = 0; n < command.ElemCount; n++)
                    {
                        const unsigned int index = static_cast<unsigned int>(indices[n]);
                        min_index = (std::min)(min_index, index);
                        max_index = (std::max)(max_index, index);
                    }
                    if (min_index > max_index)
                    {
                        continue;
                    }

                    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                                 command.VtxOffset + vertex_base, min_index,
                                                 max_index - min_index + 1,
                                                 command.IdxOffset + index_base,
                                                 command.ElemCount / 3);
                    counts->declared_vertices += max_index - min_index + 1;
                }

                counts->commands++;
            }

            index_base += static_cast<UINT>(list->IdxBuffer.Size);
            vertex_base += list_vertices;
        }

        counts->vertices = static_cast<unsigned int>(draw_data->TotalVtxCount);
    }

    // ---------------------------------------------------------------- scene

    // A row of an ordinary data table: text cells, a framed button, two custom-drawn switches and
    // two flat button labels. Realistic vertex density for a UI, with nothing application-specific
    // in it.
    void switch_widget(float height, bool on)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = height * 1.7f;
        ImGui::InvisibleButton("##switch", ImVec2(width, height));

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                                 on ? IM_COL32(80, 200, 120, 255) : IM_COL32(70, 70, 70, 255),
                                 height * 0.5f);

        const float radius = height * 0.5f - 1.8f;
        const ImVec2 center(on ? origin.x + width - height * 0.5f : origin.x + height * 0.5f,
                            origin.y + height * 0.5f);

        draw_list->AddCircleFilled(center, radius, IM_COL32(230, 230, 230, 255), 8);
    }

    void build_scene(int rows, int extra_commands, bool per_row_clip)
    {
        ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("bench", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        static const char *headers[8] = {"#", "Name", "Size", "Format", "Enabled",
                                         "Visible", "Usage", "Actions"};

        if (ImGui::BeginTable("data", 8,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            for (int column = 0; column < 8; column++)
            {
                ImGui::TableSetupColumn(headers[column]);
            }
            ImGui::TableHeadersRow();

            for (int row = 0; row < rows; row++)
            {
                ImGui::TableNextRow();

                // A clip rect per row splits the row's geometry into a command of its own: many
                // commands, each spanning a small part of the vertex buffer. That is the shape
                // the dashboard produces with its per-column clipping.
                if (per_row_clip)
                {
                    const ImVec2 row_start = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->PushClipRect(
                        ImVec2(row_start.x - 2.0f, row_start.y - 2.0f),
                        ImVec2(row_start.x + 600.0f, row_start.y + ImGui::GetFrameHeight() + 2.0f),
                        true);
                }

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", row + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(
                    "archive/backup/2026-11/dataset_with_a_long_name.csv");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("1.2 KB");
                ImGui::TableSetColumnIndex(3);
                ImGui::Button("Standard");
                ImGui::TableSetColumnIndex(4);
                switch_widget(ImGui::GetFrameHeight(), (row & 1) == 0);
                ImGui::TableSetColumnIndex(5);
                switch_widget(ImGui::GetFrameHeight(), false);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("1/17");
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("View");
                ImGui::SameLine();
                ImGui::Text("Apply");

                if (per_row_clip)
                {
                    ImGui::GetWindowDrawList()->PopClipRect();
                }
            }

            ImGui::EndTable();
        }

        // Each clip rect change starts a new draw command, and each command pays the declared
        // vertex range again. This is how the command count is swept independently of the
        // vertex count.
        ImDrawList *draw_list = ImGui::GetWindowDrawList();

        for (int i = 0; i < extra_commands; i++)
        {
            // Top-left corner of the window: inside the window's clip rect, so the command is
            // not culled, and each one gets a clip rect of its own.
            const float x = 4.0f + i * 7.0f;
            draw_list->PushClipRect(ImVec2(x, 4.0f), ImVec2(x + 5.0f, 9.0f), true);
            draw_list->AddRectFilled(ImVec2(x, 4.0f), ImVec2(x + 5.0f, 9.0f), IM_COL32(255, 0, 0, 255));
            draw_list->PopClipRect();
        }

        ImGui::End();
    }

    // ---------------------------------------------------------------- measurement

    struct Timings
    {
        double submit_ms = 0.0;
        double present_ms = 0.0;
    };

    Timings submit_once(SubmitState &state, const ImDrawData *draw_data, bool exact_vertex_range,
                        DrawCounts *counts)
    {
        IDirect3DDevice9 *device = state.device;

        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(32, 32, 32), 1.0f, 0);
        device->BeginScene();
        upload(state, draw_data);
        setup_render_state(state, draw_data);

        const Clock::time_point submit_start = Clock::now();
        draw(state, draw_data, exact_vertex_range, counts);
        const Clock::time_point submit_end = Clock::now();

        device->EndScene();

        const Clock::time_point present_start = Clock::now();
        device->Present(nullptr, nullptr, nullptr, nullptr);
        const Clock::time_point present_end = Clock::now();

        Timings timings;
        timings.submit_ms =
            std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
        timings.present_ms =
            std::chrono::duration<double, std::milli>(present_end - present_start).count();
        return timings;
    }

    // Median of the per-frame submit time, with the present time averaged alongside it.
    Timings measure(SubmitState &state, const ImDrawData *draw_data, bool exact_vertex_range,
                    int frames)
    {
        DrawCounts counts;
        for (int warmup = 0; warmup < 5; warmup++)
        {
            submit_once(state, draw_data, exact_vertex_range, &counts);
        }

        std::vector<double> submit;
        std::vector<double> present;
        submit.reserve(frames);
        present.reserve(frames);

        for (int frame = 0; frame < frames; frame++)
        {
            pump_messages();
            const Timings timings = submit_once(state, draw_data, exact_vertex_range, &counts);
            submit.push_back(timings.submit_ms);
            present.push_back(timings.present_ms);
        }

        std::sort(submit.begin(), submit.end());
        std::sort(present.begin(), present.end());

        Timings result;
        result.submit_ms = submit[submit.size() / 2];
        result.present_ms = present[present.size() / 2];

        return result;
    }

    struct Config
    {
        const char *label;
        int rows;
        int extra_commands;
        bool per_row_clip;
    };

    struct Row
    {
        Config config;
        DrawCounts upstream_counts;
        DrawCounts exact_counts;
        double upstream_submit;
        double upstream_present;
        double exact_submit;
        double exact_present;
    };

    void print_table(const std::vector<Row> &rows)
    {
        std::printf("\n%-22s %6s %5s %10s %10s %7s %10s %10s %8s\n", "scene", "vtx", "cmds",
                    "declared", "declared", "less", "upstream", "exact", "speedup");
        std::printf("%-22s %6s %5s %10s %10s %7s %10s %10s %8s\n", "", "", "", "upstream",
                    "exact", "work", "submit", "submit", "");
        std::printf("%s\n", std::string(106, '-').c_str());

        for (const Row &row : rows)
        {
            const double speedup =
                row.exact_submit > 0.0 ? row.upstream_submit / row.exact_submit : 0.0;
            const double less = row.exact_counts.declared_vertices > 0
                                    ? static_cast<double>(row.upstream_counts.declared_vertices) /
                                          row.exact_counts.declared_vertices
                                    : 0.0;
            std::printf("%-22s %6u %5u %10u %10u %6.2fx %8.2fms %8.2fms %7.2fx\n", row.config.label,
                        row.upstream_counts.vertices, row.upstream_counts.commands,
                        row.upstream_counts.declared_vertices, row.exact_counts.declared_vertices,
                        less, row.upstream_submit, row.exact_submit, speedup);
        }
    }

} // namespace

int main(int argc, char **argv)
{
    bool hardware_vertex_processing = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--hwvp") == 0)
        {
            hardware_vertex_processing = true;
        }
        else if (std::strcmp(argv[i], "--swvp") == 0)
        {
            hardware_vertex_processing = false;
        }
    }

    if (!create_window())
    {
        std::printf("could not create the window\n");
        return 1;
    }

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        std::printf("Direct3DCreate9 failed\n");
        return 1;
    }

    D3DPRESENT_PARAMETERS present = {};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_X8R8G8B8;
    present.BackBufferWidth = kWidth;
    present.BackBufferHeight = kHeight;
    present.BackBufferCount = 1;
    present.hDeviceWindow = g_window;
    present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    const DWORD behavior = hardware_vertex_processing ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                                                      : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

    IDirect3DDevice9 *device = nullptr;
    const HRESULT created =
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, behavior, &present,
                          &device);

    if (FAILED(created) || !device)
    {
        std::printf("CreateDevice failed (0x%08lX) with %s vertex processing\n",
                    static_cast<unsigned long>(created),
                    hardware_vertex_processing ? "hardware" : "software");
        return 1;
    }

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    device->GetCreationParameters(&creation);
    D3DADAPTER_IDENTIFIER9 identifier = {};
    d3d->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &identifier);
    D3DCAPS9 caps = {};
    device->GetDeviceCaps(&caps);

    std::printf("adapter   : %s\n", identifier.Description);
    std::printf("device    : %s, %s vertex processing\n", device_type_name(creation.DeviceType),
                (creation.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING)
                    ? "SOFTWARE"
                    : "hardware");

    std::printf("backbuffer: %ux%u\n", present.BackBufferWidth, present.BackBufferHeight);
    std::printf("hw T&L    : %s\n",
                (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "yes" : "no");

    // Dear ImGui, legacy texture path: no ImGuiBackendFlags_RendererHasTextures, so the atlas
    // is a single texture we create ourselves.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;

    SubmitState state;
    state.device = device;

    unsigned char *pixels = nullptr;
    int texture_width = 0;
    int texture_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texture_width, &texture_height);
    device->CreateTexture(static_cast<UINT>(texture_width), static_cast<UINT>(texture_height), 1,
                          D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                          &state.font_texture, nullptr);

    D3DLOCKED_RECT locked = {};
    state.font_texture->LockRect(0, &locked, nullptr, 0);
    for (int y = 0; y < texture_height; y++)
    {
        std::memcpy(static_cast<unsigned char *>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                    pixels + static_cast<size_t>(y) * texture_width * 4,
                    static_cast<size_t>(texture_width) * 4);
    }

    state.font_texture->UnlockRect(0);
    io.Fonts->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(state.font_texture)));

    const Config configs[] = {
        {"menu chrome only", 0, 0, false},
        {"1 row", 1, 0, false},
        {"5 rows", 5, 0, false},
        {"20 rows", 20, 0, false},
        {"40 rows", 40, 0, false},
        {"100 rows", 100, 0, false},
        {"20 rows, +8 cmds", 20, 8, false},
        {"20 rows, +24 cmds", 20, 24, false},
        {"20 rows, per-row clip", 20, 0, true},
        {"40 rows, per-row clip", 40, 0, true},
    };

    std::vector<Row> rows;

    for (const Config &config : configs)
    {
        // One frame, submitted many times: geometry and command list are identical between
        // the two variants, so only the vertex range can explain a difference.
        ImGui::NewFrame();
        build_scene(config.rows, config.extra_commands, config.per_row_clip);
        ImGui::Render();
        const ImDrawData *draw_data = ImGui::GetDrawData();

        // Count what each variant would declare, without timing it.
        DrawCounts upstream_counts;
        DrawCounts exact_counts;
        submit_once(state, draw_data, false, &upstream_counts);
        submit_once(state, draw_data, true, &exact_counts);

        double upstream_submit = 1e9;
        double upstream_present = 1e9;
        double exact_submit = 1e9;
        double exact_present = 1e9;
        for (int repeat = 0; repeat < kRepeats; repeat++)
        {
            const Timings upstream = measure(state, draw_data, false, kMeasureFrames);
            const Timings exact = measure(state, draw_data, true, kMeasureFrames);
            upstream_submit = (std::min)(upstream_submit, upstream.submit_ms);
            upstream_present = (std::min)(upstream_present, upstream.present_ms);
            exact_submit = (std::min)(exact_submit, exact.submit_ms);
            exact_present = (std::min)(exact_present, exact.present_ms);
        }

        Row row;
        row.config = config;
        row.upstream_counts = upstream_counts;
        row.exact_counts = exact_counts;
        row.upstream_submit = upstream_submit;
        row.upstream_present = upstream_present;
        row.exact_submit = exact_submit;
        row.exact_present = exact_present;
        rows.push_back(row);

        std::printf(
            "%-22s vtx %6u cmds %3u  declared %8u -> %8u  upstream %7.2fms  exact %7.2fms\n",
            config.label, upstream_counts.vertices, upstream_counts.commands,
            upstream_counts.declared_vertices, exact_counts.declared_vertices, upstream_submit,
            exact_submit);
    }

    std::printf("\n=== submit time (median of %d frames, best of %d repeats) ===\n",
                kMeasureFrames, kRepeats);
    print_table(rows);

    std::printf("\nvertices pushed through the transform stage per frame (%s vertex processing):\n",
                hardware_vertex_processing ? "hardware" : "software");

    for (const Row &row : rows)
    {
        const double less = row.exact_counts.declared_vertices > 0
                                ? static_cast<double>(row.upstream_counts.declared_vertices) /
                                      row.exact_counts.declared_vertices
                                : 0.0;
        const double redundant = row.upstream_counts.declared_vertices > 0
                                     ? 100.0 * (1.0 - static_cast<double>(row.exact_counts.declared_vertices) /
                                                          row.upstream_counts.declared_vertices)
                                     : 0.0;
        std::printf("  %-22s %8u -> %8u  (%.1fx less work, %.0f%% of it was redundant)\n",
                    row.config.label, row.upstream_counts.declared_vertices,
                    row.exact_counts.declared_vertices, less, redundant);
    }

    if (state.vertex_buffer)
    {
        state.vertex_buffer->Release();
    }

    if (state.index_buffer)
    {
        state.index_buffer->Release();
    }

    state.font_texture->Release();
    ImGui::DestroyContext();
    device->Release();
    d3d->Release();
    DestroyWindow(g_window);

    return 0;
}
