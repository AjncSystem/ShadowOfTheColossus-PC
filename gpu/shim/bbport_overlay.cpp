// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save/load slot list (OpenSaveList), guarded by imgui_mutex.
bool list_active = false;
std::string list_title;
std::vector<std::string> list_items;
int list_selected = 0, list_result = -1;
int list_confirm = -1;  // item waiting for the delete confirmation, or -1
int list_first_deletable = 0;
bool list_scroll = false, list_stick_down = false, list_stick_up = false;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// The game's save/load slot list: one entry per save, the selected one highlighted.
void SaveListBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.46f, viewport->WorkSize.y * 0.62f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##save_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::PushTextWrapPos(0.0f); // sotc: save dialog messages are whole sentences
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", list_title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##items", ImVec2(0.0f, -footer));
    for (int i = 0; i < static_cast<int>(list_items.size()); ++i) {
        ImGui::PushID(i);
        const bool selected = i == list_selected;
        if (ImGui::Selectable(list_items[i].c_str(), selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            list_selected = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                list_result = i;
            }
        }
        if (selected && list_scroll) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    list_scroll = false;
    ImGui::EndChild();
    ImGui::Separator();
    if (list_confirm >= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Apagar este save?   Enter / X (A): sim    Esc / O (B): nao");
    } else if (list_first_deletable >= static_cast<int>(list_items.size())) { // sotc: YES/NO
        ImGui::TextDisabled("Setas / direcional: escolher    Enter / X (A): confirmar    "
                            "Esc / O (B): nao");
    } else {
        ImGui::TextDisabled("Setas / direcional: escolher    Enter / X (A): confirmar    "
                            "Esc / O (B): voltar    Del / Triangulo (Y): apagar");
    }
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f FPS  %.1f ms", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: ready\n");
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (list_active && list_result == -1) {
        const int count = static_cast<int>(list_items.size());
        const auto move = [&](int delta) {
            list_confirm = -1;
            if (count) {
                list_selected = (list_selected + delta + count) % count;
                list_scroll = true;
            }
        };
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            switch (event.key.key) {
            case SDLK_UP:
            case SDLK_W:
                move(-1);
                break;
            case SDLK_DOWN:
            case SDLK_S:
                move(1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                if (!event.key.repeat && list_confirm >= 0) {
                    list_result = -100 - list_confirm; // delete it (PollSaveList)
                    list_confirm = -1;
                } else if (!event.key.repeat && count) {
                    list_result = list_selected;
                }
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                if (!event.key.repeat) {
                    if (list_confirm >= 0) {
                        list_confirm = -1;
                    } else {
                        list_result = -2;
                    }
                }
                break;
            case SDLK_DELETE:
                if (!event.key.repeat && list_selected >= list_first_deletable &&
                    list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                move(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                move(1);
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                if (list_confirm >= 0) {
                    list_result = -100 - list_confirm;
                    list_confirm = -1;
                } else if (count) {
                    list_result = list_selected;
                }
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                if (list_confirm >= 0) {
                    list_confirm = -1;
                } else {
                    list_result = -2;
                }
                break;
            case SDL_GAMEPAD_BUTTON_NORTH:
                if (list_selected >= list_first_deletable && list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                const bool down = event.gaxis.value > 20000, up = event.gaxis.value < -20000;
                if (down && !list_stick_down) {
                    move(1);
                }
                if (up && !list_stick_up) {
                    move(-1);
                }
                list_stick_down = down;
                list_stick_up = up;
            }
            return true;
        case SDL_EVENT_MOUSE_MOTION: {
            const float density = PixelDensity(event.motion.windowID);
            io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                io.AddMouseButtonEvent(0, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
            return true;
        default:
            break;
        }
    }
    return false;
}

bool Visible() {
    return initialized && (text_entry_active || list_active || BbSettings::Get().show_fps);
}

bool CapturesInput() {
    return text_entry_active || list_active;
}

bool OpenSaveList(const std::string& title, const std::vector<std::string>& items, int focus,
                  int first_deletable) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false; // nothing could show it
    }
    list_title = title;
    list_items = items;
    list_selected = items.empty() ? 0 : std::clamp(focus, 0, static_cast<int>(items.size()) - 1);
    list_result = -1;
    list_confirm = -1;
    list_first_deletable = first_deletable < 0 ? static_cast<int>(items.size()) : first_deletable;
    list_scroll = true;
    list_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
    return true;
}

int PollSaveList() {
    std::scoped_lock lock{imgui_mutex};
    if (!list_active || list_result == -1) {
        return -1;
    }
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = false;
    }
    return list_result;
}

void CloseSaveList() {
    std::scoped_lock lock{imgui_mutex};
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = false;
    }
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (BbSettings::Get().show_fps) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (list_active) {
        SaveListBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
