/*
This file (Plugin.cpp) is licensed under the MIT license and is separate from the rest of the UEVR codebase.

Copyright (c) 2023 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <sstream>
#include <mutex>
#include <memory>
#include <locale>
#include <codecvt>

#include <Windows.h>

// only really necessary if you want to render to the screen
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

#include <C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\include\nvml.h>

using namespace uevr;

#define PLUGIN_LOG_ONCE(...) \
    static bool _logged_ = false; \
    if (!_logged_) { \
        _logged_ = true; \
        API::get()->log_info(__VA_ARGS__); \
    }

class ExamplePlugin : public uevr::Plugin {
public:
    ExamplePlugin() = default;

    void on_dllmain() override {}

    void on_initialize() override {
        ImGui::CreateContext();
    }

    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        if (!m_initialized) {
            if (!initialize_imgui()) {
                API::get()->log_info("Failed to initialize imgui");
                return;
            } else {
                API::get()->log_info("Initialized imgui");
            }
        }

        const auto renderer_data = API::get()->param()->renderer;

        if (!API::get()->param()->vr->is_hmd_active()) {
            if (!m_was_rendering_desktop) {
                m_was_rendering_desktop = true;
                on_device_reset();
                return;
            }

            m_was_rendering_desktop = true;

            if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
                ImGui_ImplDX11_NewFrame();
                g_d3d11.render_imgui();
            } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
                auto command_queue = (ID3D12CommandQueue*)renderer_data->command_queue;

                if (command_queue == nullptr) {
                    return;
                }

                ImGui_ImplDX12_NewFrame();
                g_d3d12.render_imgui();
            }
        }
    }    

    void on_device_reset() override {
        PLUGIN_LOG_ONCE("Example Device Reset");

        std::scoped_lock _{m_imgui_mutex};

        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        m_initialized = false;
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {
        PLUGIN_LOG_ONCE("Post Render VR Framework DX11");

        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (!m_initialized || !vr_active) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        PLUGIN_LOG_ONCE("Post Render VR Framework DX12");

        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (!m_initialized || !vr_active) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(command_list, rtv);
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override { 
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }                    

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        PLUGIN_LOG_ONCE("Pre Engine Tick: %f", delta);        

        if (m_initialized) {
            std::scoped_lock _{m_imgui_mutex};

            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            API::get()->log_info("Running imgui internal_frame");
            internal_frame();

            ImGui::EndFrame();
            ImGui::Render();
        }
    }        

private:
    bool initialize_imgui() {
        API::get()->log_info("Init imgui");

        if (m_initialized) {
            return true;
        }

        std::scoped_lock _{m_imgui_mutex};

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        static const auto imgui_ini = API::get()->get_persistent_dir(L"imgui_example_plugin.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        const auto renderer_data = API::get()->param()->renderer;

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
        swapchain->GetDesc(&swap_desc);

        m_wnd = swap_desc.OutputWindow;

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        }

        API::get()->log_info("Init imgui done");

        m_initialized = true;
        return true;
    }

    int get_gpu_usage() {
        static bool initialized = false;
        static nvmlDevice_t device;

        if (!initialized) {
            API::get()->log_info("Init start");
            if (nvmlInit() != NVML_SUCCESS)
                return -1;
            if (nvmlDeviceGetHandleByIndex(0, &device) != NVML_SUCCESS)
                return -1;
            initialized = true;
            API::get()->log_info("Init done");
        }

        nvmlUtilization_t utilization;
        if (nvmlDeviceGetUtilizationRates(device, &utilization) == NVML_SUCCESS) {
            API::get()->log_info("Returning usage");
            return utilization.gpu;
        }

        return -1;
    }
    
    void internal_frame() {
        API::get()->log_info("Internal frame start");
        if (ImGui::Begin("Super Cool Plugin")) {
            API::get()->log_info("Internal frame in plugin");
            ImGui::Text("Hello from the super cool plugin!");
            ImGui::Text("Snap turn: %i", API::VR::is_snap_turn_enabled());
            ImGui::Text("Decoupled pitch: %i", API::VR::is_decoupled_pitch_enabled());
            if (ImGui::Button("Toggle snap turn")) {
                API::VR::set_snap_turn_enabled(!API::VR::is_snap_turn_enabled());
            }

            if (ImGui::Button("Toggle decoupled pitch")) {
                API::VR::set_decoupled_pitch_enabled(!API::VR::is_decoupled_pitch_enabled());
            }

            if (ImGui::Button("Screw up world scale")) {
                API::VR::set_mod_value("VR_WorldScale", 1.337f);
            }

            if (ImGui::Button("Toggle GUI")) {
                const bool enabled = API::VR::get_mod_value<bool>("VR_EnableGUI");
                API::VR::set_mod_value("VR_EnableGUI", !enabled);
            }

            static char input[256]{};
            if (ImGui::InputText("Get mod value", input, sizeof(input))) {

            }

            std::string mod_value = API::VR::get_mod_value<std::string>(input);
            ImGui::Text("Mod value: %s", mod_value.c_str());

            if (ImGui::Button("Save Config")) {
                API::VR::save_config();
            }

            if (ImGui::Button("Reload Config")) {
                API::VR::reload_config();
            }

            if (ImGui::Button("Toggle UObjectHook disabled")) {
                const auto value = API::UObjectHook::is_disabled();

                API::UObjectHook::set_disabled(!value);
            }            
            ImGui::Text("GPU usage is %d%%", get_gpu_usage());
    #if defined(__clang__)
            ImGui::Text("Plugin Compiler: Clang");
    #elif defined(_MSC_VER)
            ImGui::Text("Plugin Compiler: Visual Studio");
    #elif defined(__GNUC__)
            ImGui::Text("Plugin Compiler: GCC");
    #else
            ImGui::Text("Plugin Compiler: Unknown");
    #endif
        }
        API::get()->log_info("Internal frame done");
    }

    

private:
    HWND m_wnd{};
    bool m_initialized{false};
    bool m_was_rendering_desktop{false};

    std::recursive_mutex m_imgui_mutex{};
};

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<ExamplePlugin> g_plugin{new ExamplePlugin()};
