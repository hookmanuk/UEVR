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
#include <filesystem>

// only really necessary if you want to render to the screen
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

#include <C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\include\nvml.h>
#include "json.hpp"
#include <fstream>

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
        configpath = API::get()->get_persistent_dir(L"autoscalerconfig.json").string();    
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

            //API::get()->log_info("Running imgui internal_frame");
            internal_frame();

            ImGui::EndFrame();
            ImGui::Render();
        }
    }        

    void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
        sinceincrease = sinceincrease + delta;
        sincedecrease = sincedecrease + delta;
        int usage = get_gpu_usage();
        if (usage == -1) {
            return;
        }
        // aim to keep the usage between 82 & 92 percent
        // increase infrequently only by 5%, every 5 seconds at most, to prevent too many hitches
        // decrease often and by 10%, every 0.5 seconds if needed, so we're not below target too long
        if (usage <= usagelowerbound && screenpercentage < 100) {
            framesunderbudget = framesunderbudget + 1;
            if (framesunderbudget > increaseframesrequired) {
                screenpercentage = screenpercentage + increaseresamount;
                std::wstring command = L"r.ScreenPercentage ";
                command.append(std::to_wstring(screenpercentage));
                API::get()->sdk()->functions->execute_command(command.c_str());
                lastchange = std::format("Increased res to: {}%% after {:.2f} secs. Usage was {}%%", static_cast<int>(screenpercentage),
                    static_cast<float>(sinceincrease), static_cast<int>(usage));
                API::get()->log_info(lastchange.c_str());
                sinceincrease = 0;
                framesunderbudget = 0;
                lastchange_time = std::time(nullptr);
            }
        } else {
            framesunderbudget = 0;
        }
        if (usage >= usageupperbound && screenpercentage >= 20) {
            framesoverbudget = framesoverbudget + 1;
            if (framesoverbudget > decreaseframesrequired) {
                screenpercentage = screenpercentage - decreaseresamount;
                std::wstring command = L"r.ScreenPercentage ";
                command.append(std::to_wstring(screenpercentage));
                API::get()->sdk()->functions->execute_command(command.c_str());
                lastchange = std::format("Decreased res to:{}%% after {:.2f} secs. Usage was {}%%", static_cast<int>(screenpercentage), static_cast<float>(sinceincrease), static_cast<int>(usage));
                API::get()->log_info(lastchange.c_str());                
                sincedecrease = 0;
                framesoverbudget = 0;
                lastchange_time = std::time(nullptr);
            }
        } else {
            framesoverbudget = 0;
        }
    }  

private:
    int screenpercentage = 50;
    float sinceincrease = 0;
    float sincedecrease = 0;
    int framesoverbudget = 0;
    int framesunderbudget = 0;
    int usagelowerbound = 82;
    int usageupperbound = 92;
    int decreaseresamount = 2;
    int increaseresamount = 1;    
    int decreaseframesrequired = 10;
    int increaseframesrequired = 20;
    std::string lastchange = "";
    std::time_t lastchange_time = std::time(0);
    std::string configpath = "";

    std::string get_dll_directory() {
        HMODULE hModule = GetModuleHandle(NULL); // Get the handle of the current module (DLL or EXE)

        if (hModule == nullptr) {
            return ""; // Failed to get the module handle
        }

        char path[MAX_PATH] = {};                    // Buffer to store the path
        GetModuleFileNameA(hModule, path, MAX_PATH); // Get the full path of the module (DLL or EXE)

        // Use filesystem to extract the directory from the path
        return std::filesystem::path(path).parent_path().string();
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
            //API::get()->log_info("Returning usage");
            return utilization.gpu;
        }

        return -1;
    }

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

    // Load values from JSON config file
    void load_config() {
        std::ifstream configFile(configpath);
        if (configFile.is_open()) {
            nlohmann::json j;
            configFile >> j;            

            if (j.contains("usagelowerbound")) {
                usagelowerbound = j["usagelowerbound"];
            }
            if (j.contains("usageupperbound")) {
                usageupperbound = j["usageupperbound"];
            }
            if (j.contains("decreaseframesrequired")) {
                usageupperbound = j["decreaseframesrequired"];
            }
            if (j.contains("increaseframesrequired")) {
                usageupperbound = j["increaseframesrequired"];
            }
            if (j.contains("decreaseresamount")) {
                usageupperbound = j["decreaseresamount"];
            }
            if (j.contains("increaseresamount")) {
                usageupperbound = j["increaseresamount"];
            }   
        }
    }

    void save_config() {
        nlohmann::json j;
        j["usagelowerbound"] = usagelowerbound;        
        j["usageupperbound"] = usageupperbound;
        j["decreaseframesrequired"] = decreaseframesrequired;
        j["increaseframesrequired"] = increaseframesrequired;
        j["decreaseresamount"] = decreaseresamount;
        j["increaseresamount"] = increaseresamount;        

        API::get()->log_info("Saving config");
        std::ofstream configFile(configpath);
        if (configFile.is_open()) {
            API::get()->log_info("Opened file");
            configFile << j.dump(4); // Pretty print with 4-space indent
        }
    }
    
    void internal_frame() {
        //API::get()->log_info("Internal frame start");
        if (ImGui::Begin("Autoscaler")) {
            //API::get()->log_info("Internal frame in plugin");                   

            static char input[256]{};
            
            bool changed = false;

            if (ImGui::SliderInt("Usage Lower Bound", &usagelowerbound, 60, 95)) {
                changed = true;
                if (usageupperbound <= usagelowerbound + 5) {
                    usageupperbound = usagelowerbound + 5;
                }
            }
            if (ImGui::SliderInt("Usage Upper Bound", &usageupperbound, 60, 95)) {
                changed = true;
                if (usagelowerbound >= usageupperbound - 5) {
                    usagelowerbound = usageupperbound - 5;
                }
            }            
            if (ImGui::SliderInt("Frames Before Decreasing", &decreaseframesrequired, 1, 1000)) {
                changed = true;                
            }
            if (ImGui::SliderInt("Frames Before Increasing", &increaseframesrequired, 1, 1000)) {
                changed = true;
            }
            if (ImGui::SliderInt("Decrease Res By", &decreaseresamount, 1, 20)) {
                changed = true;
            }
            if (ImGui::SliderInt("Increase Res By", &increaseresamount, 1, 20)) {
                changed = true;
            }


            if (changed) {
                save_config();
            }
                   
            if (lastchange_time != 0) {
                // Get current time
                std::time_t now = std::time(nullptr);

                // Calculate the difference in seconds
                double seconds_diff = std::difftime(now, lastchange_time);
                
                // Print formatted time using ImGui
                ImGui::Text("Last changed %.0f secs ago", seconds_diff);
            }
            ImGui::Text(lastchange.c_str());     
            ImGui::Text("GPU usage is %d%%", get_gpu_usage());   
        }
        //API::get()->log_info("Internal frame done");
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
