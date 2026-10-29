#include "editor_shell.hpp"
#include "gui_adapter.hpp"
#include "project_session.hpp"
#include "workspace_panel.hpp"

#include <shark/client/shader_bundle.hpp>
#include <shark/time.hpp>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <set>

namespace shark::editor {
namespace {
using namespace client;
using Json = nlohmann::json;
struct Failure {
    Error error;
};
template <class T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::filesystem::path native_path(std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
template <std::size_t N> void buffer(std::array<char, N>& output, std::string_view value) {
    if (value.size() >= N)
        throw Failure{{ErrorCode::invalid_path,
                       "Editor path exceeds its input bound",
                       {.operation = "editor.path"}}};
    std::copy(value.begin(), value.end(), output.begin());
    output[value.size()] = '\0';
}
struct GuiContext {
    ImGuiContext* value{ImGui::CreateContext()};
    GuiContext() {
        if (!value)
            throw Failure{{ErrorCode::execution_failed,
                           "Cannot create editor UI",
                           {.operation = "editor.gui.create"}}};
    }
    ~GuiContext() {
        ImGui::DestroyContext(value);
    }
};
void style(float scale) {
    ImGui::StyleColorsDark();
    auto& value = ImGui::GetStyle();
    value.WindowRounding = 0;
    value.ChildRounding = 6;
    value.FrameRounding = 4;
    value.PopupRounding = 5;
    value.FramePadding = {10, 7};
    value.ItemSpacing = {10, 10};
    value.WindowPadding = {22, 20};
    value.Colors[ImGuiCol_WindowBg] = {0.055F, 0.075F, 0.09F, 1};
    value.Colors[ImGuiCol_ChildBg] = {0.08F, 0.105F, 0.125F, 1};
    value.Colors[ImGuiCol_FrameBg] = {0.12F, 0.155F, 0.18F, 1};
    value.Colors[ImGuiCol_Button] = {0.10F, 0.32F, 0.37F, 1};
    value.Colors[ImGuiCol_ButtonHovered] = {0.13F, 0.43F, 0.48F, 1};
    value.Colors[ImGuiCol_ButtonActive] = {0.10F, 0.37F, 0.43F, 1};
    value.Colors[ImGuiCol_Header] = {0.10F, 0.32F, 0.37F, 1};
    value.Colors[ImGuiCol_CheckMark] = {0.35F, 0.82F, 0.77F, 1};
    value.Colors[ImGuiCol_Text] = {0.91F, 0.95F, 0.96F, 1};
    value.Colors[ImGuiCol_TextDisabled] = {0.52F, 0.62F, 0.67F, 1};
    value.ScaleAllSizes(scale);
}

class Shell {
  public:
    Shell(const EditorLaunch& launch, const ShaderArtifact& scene_vertex,
          const ShaderArtifact& scene_pixel)
        : scene_vertex_(scene_vertex), scene_pixel_(scene_pixel) {
        buffer(root_, utf8(launch.root));
        buffer(project_, launch.project);
        if (!launch.project.empty())
            open();
    }
    bool exiting() const noexcept {
        return exiting_;
    }
    bool dirty() const noexcept {
        return session_ && session_->state().dirty;
    }
    void request_close(bool exit) {
        if (pending_close_ || pending_draft_close_)
            return;
        if (session_) {
            auto refreshed = session_->refresh();
            if (!refreshed) {
                show_error(std::move(refreshed).error());
                return;
            }
        }
        if (workspace_)
            workspace_->cancel_confirmation();
        exit_after_close_ = exit;
        if (workspace_ && workspace_->has_unapplied_drafts()) {
            pending_draft_close_ = true;
            return;
        }
        if (!session_ || !session_->state().open) {
            exiting_ = exit;
            return;
        }
        if (session_->state().dirty) {
            close_revision_ = session_->state().revision;
            pending_close_ = true;
        } else {
            close(CloseChoice::reject, session_->state().revision);
        }
    }
    void draw(const GraphicsWindowFrame& frame) {
        frame_invalidated_ = false;
        const bool modal = pending_close_ || pending_draft_close_ ||
                           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool text = ImGui::GetIO().WantTextInput;
        if (editor_shortcut(frame.input, Key::s, text, modal))
            save();
        if (editor_shortcut(frame.input, Key::w, text, modal))
            request_close(false);
        if (editor_shortcut(frame.input, Key::o, text, modal))
            focus_open_ = true;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("Shark editor", nullptr, flags);
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open project", "Ctrl+O", false,
                                    !is_open() && (!session_ || session_->state_current())))
                    focus_open_ = true;
                if (ImGui::MenuItem("Save scene", "Ctrl+S", false,
                                    is_open() && session_->state_current()))
                    save();
                if (ImGui::MenuItem("Close project", "Ctrl+W", false,
                                    is_open() && session_->state_current()))
                    request_close(false);
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    request_close(true);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::TextColored({0.35F, 0.82F, 0.77F, 1}, "SHARK");
        ImGui::SameLine();
        ImGui::TextDisabled("/  EDITOR");
        ImGui::Spacing();
        ImGui::TextUnformatted(is_open() ? session_->state().name.c_str() : "Open your project");
        if (is_open()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", dirty() ? "Unsaved changes" : "Saved");
        }
        ImGui::Spacing();
        if (session_ && !session_->state_current()) {
            ImGui::TextColored({1, .73F, .34F, 1}, "Project status needs refresh");
            if (ImGui::Button("Refresh project status")) {
                const auto refreshed = session_->refresh();
                if (!refreshed)
                    show_error(refreshed.error());
                else
                    error_.reset();
            }
        }
        if (!is_open() || ImGui::CollapsingHeader("Project")) {
            ImGui::TextUnformatted("PROJECT");
            ImGui::Separator();
            ImGui::BeginDisabled(is_open() || (session_ && !session_->state_current()));
            ImGui::TextUnformatted("Project folder");
            ImGui::SetNextItemWidth(-1);
            if (focus_open_ && !is_open()) {
                ImGui::SetKeyboardFocusHere();
                focus_open_ = false;
            }
            ImGui::InputText("##project-root", root_.data(), root_.size());
            ImGui::TextUnformatted("Project file (relative to folder)");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##project-file", project_.data(), project_.size());
            if (ImGui::Button("Open project"))
                open();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!is_open() || !session_->state_current());
            if (ImGui::Button("Save scene"))
                save();
            ImGui::SameLine();
            if (ImGui::Button("Close project"))
                request_close(false);
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::Separator();
            if (is_open()) {
                const auto& state = session_->state();
                ImGui::TextColored(state.dirty ? ImVec4{1, .73F, .34F, 1}
                                               : ImVec4{.35F, .82F, .77F, 1},
                                   "%s", state.dirty ? "Unsaved changes" : "Saved");
                ImGui::TextDisabled("Scene %s", state.scene_id.c_str());
                ImGui::TextDisabled("Revision %llu",
                                    static_cast<unsigned long long>(state.revision.revision));
                ImGui::Spacing();
                ImGui::TextWrapped("This project is owned by the editor. Save and close it before "
                                   "opening it in a terminal host.");
            } else {
                ImGui::TextDisabled("No project is open");
            }
        }
        if (!notice_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored({.35F, .82F, .77F, 1}, "%s", notice_.c_str());
        }
        if (error_) {
            ImGui::Spacing();
            ImGui::TextColored({1, .55F, .43F, 1}, "%s", error_->message.c_str());
            const auto code = error_code_name(error_->code);
            ImGui::TextDisabled("%.*s", static_cast<int>(code.size()), code.data());
            if (!error_->context.path.empty())
                ImGui::TextWrapped("%s", error_->context.path.c_str());
        }
        if (is_open() && workspace_)
            workspace_->draw(frame, pending_close_ || pending_draft_close_);
        ImGui::End();
        if (pending_draft_close_) {
            ImGui::OpenPopup("Unapplied authoring drafts");
            ImGui::SetNextWindowSize({460 * scale_, 0}, ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Unapplied authoring drafts", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("Inspector or asset metadata drafts contain unapplied edits. "
                                   "Scene Save does not apply these drafts. Keep editing to apply "
                                   "the inspector or explicitly write metadata.");
                if (ImGui::Button("Keep editing")) {
                    pending_draft_close_ = false;
                    exit_after_close_ = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard drafts and continue")) {
                    auto discarded = workspace_ ? workspace_->discard_drafts() : Result<void>{};
                    if (!discarded)
                        show_error(discarded.error());
                    else {
                        pending_draft_close_ = false;
                        ImGui::CloseCurrentPopup();
                        request_close(exit_after_close_);
                    }
                }
                if (error_)
                    ImGui::TextWrapped("%s", error_->message.c_str());
                ImGui::EndPopup();
            }
        }
        if (pending_close_) {
            ImGui::OpenPopup("Unsaved changes");
            ImGui::SetNextWindowSize({460 * scale_, 0}, ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Unsaved changes", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("Save this scene before closing the project?");
                ImGui::Spacing();
                if (ImGui::Button("Save and close"))
                    close(CloseChoice::save, close_revision_);
                ImGui::SameLine();
                if (ImGui::Button("Discard changes"))
                    close(CloseChoice::discard, close_revision_);
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    close(CloseChoice::cancel, close_revision_);
                if (error_)
                    ImGui::TextWrapped("%s", error_->message.c_str());
                if (!pending_close_)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }
    void scale(float value) noexcept {
        scale_ = value;
    }
    bool frame_invalidated() const noexcept {
        return frame_invalidated_;
    }
    const SceneViewportRequest* viewport() const noexcept {
        return workspace_ ? workspace_->viewport() : nullptr;
    }

  private:
    bool is_open() const noexcept {
        return session_ && session_->state().open;
    }
    void show_error(Error error) {
        error_ = std::move(error);
        notice_.clear();
    }
    void open() {
        if (is_open() || (session_ && !session_->state_current()))
            return;
        try {
            auto candidate = ProjectSession::create(native_path(root_.data()));
            if (!candidate) {
                show_error(std::move(candidate).error());
                return;
            }
            auto opened = candidate.value()->open(project_.data());
            if (!opened) {
                show_error(std::move(opened).error());
                return;
            }
            auto candidate_workspace =
                std::make_unique<WorkspacePanel>(*candidate.value(), scene_vertex_, scene_pixel_);
            buffer(root_, utf8(candidate.value()->root()));
            workspace_.reset();
            session_ = std::move(candidate).value();
            workspace_ = std::move(candidate_workspace);
            error_.reset();
            notice_ = "Project opened";
        } catch (...) {
            show_error({ErrorCode::invalid_path,
                        "Cannot open the selected project folder",
                        {.operation = "editor.project.open"}});
        }
    }
    void save() {
        if (!is_open() || !session_->state_current())
            return;
        auto saved = session_->save(session_->state().revision);
        if (!saved) {
            show_error(std::move(saved).error());
            return;
        }
        error_.reset();
        report_save(saved.value());
        notice_ = saved.value().durability == "directory_flush_failed"
                      ? "Scene published; directory durability could not be confirmed"
                      : "Scene saved";
        if (workspace_ && workspace_->has_unapplied_drafts())
            notice_ += "; authoring drafts remain unapplied";
    }
    void close(CloseChoice choice, SceneRevision revision) {
        if (!is_open()) {
            exiting_ = exit_after_close_;
            pending_close_ = false;
            return;
        }
        auto closed = session_->close(choice, revision);
        if (!closed) {
            show_error(std::move(closed).error());
            return;
        }
        if (closed.value().saved)
            report_save(*closed.value().saved);
        if (closed.value().error) {
            show_error(*closed.value().error);
            if (closed.value().saved)
                notice_ = "Scene published; project close did not complete";
            return;
        }
        if (!closed.value().closed && !closed.value().cancelled)
            return;
        pending_close_ = false;
        error_.reset();
        notice_.clear();
        if (closed.value().cancelled) {
            exit_after_close_ = false;
            return;
        }
        workspace_.reset();
        frame_invalidated_ = true;
        exiting_ = exit_after_close_;
        exit_after_close_ = false;
        notice_ = "Project closed";
        if (closed.value().saved && closed.value().saved->durability == "directory_flush_failed") {
            exiting_ = false;
            notice_ =
                "Project closed and scene published; directory durability could not be confirmed";
        }
    }
    void report_save(const SaveReceipt& receipt) {
        std::cout << Json{{"event", "editor_saved"},
                          {"published", receipt.published},
                          {"current_scene_clean", receipt.current_scene_clean},
                          {"durability", receipt.durability},
                          {"bytes", receipt.bytes},
                          {"scene_instance", std::to_string(receipt.saved_revision.scene_instance)},
                          {"revision", std::to_string(receipt.saved_revision.revision)}}
                         .dump()
                  << '\n'
                  << std::flush;
    }
    std::unique_ptr<ProjectSession> session_;
    std::unique_ptr<WorkspacePanel> workspace_;
    ShaderArtifact scene_vertex_;
    ShaderArtifact scene_pixel_;
    std::array<char, 4097> root_{};
    std::array<char, 4097> project_{};
    std::optional<Error> error_;
    std::string notice_;
    SceneRevision close_revision_{};
    bool focus_open_{false};
    bool pending_close_{false};
    bool pending_draft_close_{false};
    bool exit_after_close_{false};
    bool exiting_{false};
    bool frame_invalidated_{false};
    float scale_{1};
};
} // namespace

Result<EditorLaunch> parse_editor_arguments(std::span<const std::string_view> arguments) {
    const auto invalid = []() -> Result<EditorLaunch> {
        return Error{ErrorCode::invalid_argument,
                     "Expected --editor [--root PATH] [--project RELATIVE] [--device UUID|auto] "
                     "[--validation required|off] [--gui-vertex SPV --gui-pixel SPV] "
                     "[--scene-vertex SPV --scene-pixel SPV]",
                     {.operation = "editor.arguments"}};
    };
    try {
        if (arguments.empty() || arguments.front() != "--editor" || arguments.size() > 17)
            return invalid();
        EditorLaunch launch;
        launch.root = std::filesystem::current_path();
        std::set<std::string_view> seen;
        for (std::size_t index = 1; index < arguments.size(); index += 2) {
            if (index + 1 >= arguments.size() || !seen.insert(arguments[index]).second ||
                arguments[index + 1].empty() || arguments[index + 1].size() > 4096)
                return invalid();
            const auto flag = arguments[index];
            const auto value = arguments[index + 1];
            if (flag == "--root")
                launch.root = native_path(value);
            else if (flag == "--project")
                launch.project = value;
            else if (flag == "--gui-vertex")
                launch.vertex_shader = native_path(value);
            else if (flag == "--gui-pixel")
                launch.pixel_shader = native_path(value);
            else if (flag == "--scene-vertex")
                launch.scene_vertex_shader = native_path(value);
            else if (flag == "--scene-pixel")
                launch.scene_pixel_shader = native_path(value);
            else if (flag == "--device") {
                if (value != "auto" && !valid_graphics_uuid(value))
                    return invalid();
                if (value != "auto")
                    launch.adapter = value;
            } else if (flag == "--validation") {
                if (value == "required")
                    launch.validation = GraphicsValidation::required;
                else if (value == "off")
                    launch.validation = GraphicsValidation::off;
                else
                    return invalid();
            } else
                return invalid();
        }
        if (launch.vertex_shader.empty() != launch.pixel_shader.empty())
            return invalid();
        if (launch.scene_vertex_shader.empty() != launch.scene_pixel_shader.empty())
            return invalid();
        if (launch.vertex_shader.empty()) {
            // Reuse the existing executable-relative asset locator; only the
            // filenames differ from the diagnostic cube's bundle.
            auto assets = take(default_shader_bundle_path()).parent_path();
            launch.vertex_shader = assets / "gui-vs.spv";
            launch.pixel_shader = assets / "gui-ps.spv";
        }
        if (launch.scene_vertex_shader.empty()) {
            auto assets = take(default_shader_bundle_path()).parent_path();
            launch.scene_vertex_shader = assets / "scene-unlit-vs.spv";
            launch.scene_pixel_shader = assets / "scene-unlit-ps.spv";
        }
        launch.root = std::filesystem::absolute(launch.root);
        return launch;
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return invalid();
    }
}

Result<void> run_editor(const EditorLaunch& launch) {
    try {
        GuiAtlas atlas;
        atlas.vertex_shader = take(
            load_shader_artifact(launch.vertex_shader, ShaderStage::vertex, ShaderInterface::gui));
        atlas.pixel_shader = take(
            load_shader_artifact(launch.pixel_shader, ShaderStage::pixel, ShaderInterface::gui));
        const auto scene_vertex = take(load_shader_artifact(
            launch.scene_vertex_shader, ShaderStage::vertex, ShaderInterface::scene_unlit));
        const auto scene_pixel = take(load_shader_artifact(
            launch.scene_pixel_shader, ShaderStage::pixel, ShaderInterface::scene_unlit));
        Shell shell(launch, scene_vertex, scene_pixel);
        auto graphics = take(GraphicsDevice::create(
            {launch.adapter, launch.validation, {.title = "Shark Editor", .content = {900, 600}}}));
        GuiContext context;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendPlatformName = "Shark.InputSnapshot";
        io.BackendRendererName = "Shark.GraphicsDevice";
        const auto state = take(graphics->window_state());
        const auto scale = std::clamp(state.scale.y, 1.0F, 3.0F);
        style(scale);
        shell.scale(scale);
        ImFontConfig font;
        font.SizePixels = std::round(17.0F * scale);
        io.Fonts->AddFontDefault(&font);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0 || width > 2048 || height > 2048)
            return Error{ErrorCode::execution_failed,
                         "Editor font atlas exceeds its bounds",
                         {.operation = "editor.gui.font"}};
        atlas.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        atlas.rgba8.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                                static_cast<std::size_t>(height) * 4);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        take(graphics->configure_gui(atlas));
        const auto adapter = take(graphics->adapter());
        bool ready = false;
        SteadyMonotonicClock clock;
        auto previous = clock.now();
        while (!shell.exiting()) {
            const auto now = clock.now();
            auto frame = take(graphics->poll_window());
            if (frame.diagnostics.input_error)
                throw Failure{*frame.diagnostics.input_error};
            if (frame.diagnostics.window_error)
                throw Failure{*frame.diagnostics.window_error};
            if (frame.state.close_requested) {
                take(graphics->cancel_close_request());
                shell.request_close(true);
                if (shell.exiting())
                    break;
            }
            feed_gui_input(frame, std::chrono::duration<float>(now - previous).count());
            previous = now;
            ImGui::NewFrame();
            shell.draw(frame);
            ImGui::Render();
            if (shell.exiting())
                break;
            if (shell.frame_invalidated())
                continue;
            if (!frame.state.iconified && frame.state.framebuffer.width &&
                frame.state.framebuffer.height) {
                take(graphics->set_presentation_gui(take(capture_gui_draw(
                    *ImGui::GetDrawData(), frame.state.framebuffer, shell.viewport()))));
                const auto presented = take(
                    graphics->present({.red = .006F, .green = .008F, .blue = .01F, .alpha = 1}));
                if (presented.status == PresentationStatus::closed)
                    throw Failure{{ErrorCode::execution_failed,
                                   "Editor window closed before project resolution",
                                   {.operation = "editor.lifecycle"}}};
                if (!ready && presented.status == PresentationStatus::presented) {
                    std::cout << Json{{"event", "editor_ready"},
                                      {"adapter_uuid", adapter.uuid},
                                      {"validation",
                                       launch.validation == GraphicsValidation::required
                                           ? "required"
                                           : "off"}}
                                     .dump()
                              << '\n'
                              << std::flush;
                    if (!std::cout)
                        throw Failure{{ErrorCode::io_failure,
                                       "Cannot report editor readiness",
                                       {.operation = "editor.output"}}};
                    ready = true;
                }
            }
            take(clock.wait_for(std::chrono::milliseconds{8}));
        }
        take(graphics->close());
        const auto diagnostics = take(graphics->diagnostics());
        const auto stats = take(graphics->presentation_stats());
        if (diagnostics.warnings || diagnostics.errors || diagnostics.dropped || !stats.closed ||
            stats.in_flight)
            return Error{ErrorCode::execution_failed,
                         "Editor graphics did not close cleanly",
                         {.operation = "editor.lifecycle"}};
        std::cout << Json{{"event", "editor_closed"},     {"adapter_uuid", adapter.uuid},
                          {"presented", stats.presented}, {"submitted", stats.submitted},
                          {"completed", stats.completed}, {"warnings", diagnostics.warnings},
                          {"errors", diagnostics.errors}, {"dropped", diagnostics.dropped},
                          {"dirty", shell.dirty()}}
                         .dump()
                  << '\n'
                  << std::flush;
        if (!std::cout)
            return Error{ErrorCode::io_failure,
                         "Cannot report editor closure",
                         {.operation = "editor.output"}};
        return {};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "Editor shell failed", {.operation = "editor.run"}};
    }
}

} // namespace shark::editor
