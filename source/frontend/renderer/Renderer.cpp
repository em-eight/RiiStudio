#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Renderer.hpp"
#include <core/3d/gl.hpp>    // glPolygonMode
#include <frontend/root.hpp> // RootWindow
#include <imcxx/Widgets.hpp>
#include <librii/glhelper/Util.hpp> // librii::glhelper::SetGlWireframe

namespace riistudio::frontend {

const char* GetGpuName() {
  static std::string renderer =
      reinterpret_cast<const char*>(glGetString(GL_RENDERER));

  return renderer.c_str();
}

const char* GetGlVersion() {
  static std::string version =
      reinterpret_cast<const char*>(glGetString(GL_VERSION));

  return version.c_str();
}

void RenderSettings::drawMenuBar(bool draw_controller, bool draw_wireframe) {
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("Camera"_j)) {
      mCameraController.drawOptions();

      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Rendering"_j)) {
      ImGui::Checkbox("Render Scene?"_j, &rend);
      if (draw_wireframe && librii::glhelper::IsGlWireframeSupported())
        ImGui::Checkbox("Wireframe Mode"_j, &wireframe);
      ImGui::EndMenu();
    }

    ImGui::SetNextItemWidth(120.0f * ImGui::GetIO().FontGlobalScale);
    mRenderType = imcxx::EnumCombo("##mRenderType", mRenderType);

    ImGui::SetNextItemWidth(120.0f * ImGui::GetIO().FontGlobalScale);
    mCameraController.drawProjectionOption();

    {
      util::ConditionalActive a(false);

      {
        //   util::ConditionalBold g(true);
        ImGui::TextUnformatted("Backend:");
      }
      ImGui::Text("OpenGL %s", GetGlVersion());

      {
        //    util::ConditionalBold g(true);
        ImGui::TextUnformatted("Device:");
      }
      ImGui::TextUnformatted(GetGpuName());
    }

    ImGui::EndMenuBar();
  }
}

Renderer::Renderer(lib3d::IDrawable* root, const lib3d::Scene* node)
    : mRoot(root), mData(node) {
  root->dispatcher = &mRootDispatcher;
}
Renderer::~Renderer() {}

void Renderer::render(u32 width, u32 height) {
  mSettings.drawMenuBar();

  if (!mSettings.rend)
    return;

  if (!mRootDispatcher.beginDraw())
    return;

  librii::glhelper::SetGlWireframe(mSettings.wireframe);

  const auto time_step = mDeltaTimer.tick();
  if (mMouseHider.begin_interaction(ImGui::IsWindowFocused())) {
    const auto input_state = buildInputState();
    mSettings.mCameraController.move(time_step, input_state);

    mMouseHider.end_interaction(input_state.clickView);
  }

  ConfigureCameraControllerByBounds(mSettings.mCameraController,
                                    mSceneState.computeBounds());

  mSettings.mCameraController.calc();
  mSettings.mCameraController.mCamera.calcMatrices(width, height, mProjMtx,
                                                   mViewMtx);

  mSceneState.invalidate();
  assert(mData != nullptr);
  auto ok = mRootDispatcher.populate(*mRoot, mSceneState, *mData, mViewMtx,
                                     mProjMtx, mSettings.mRenderType);
  if (!ok.has_value()) {
    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_NavHighlight],
                       "Renderer error during populate(): %s",
                       ok.error().c_str());
  }
  mSceneState.buildUniformBuffers();

  librii::glhelper::ClearGlScreen();
  mSceneState.draw();

  mRootDispatcher.endDraw();
}

} // namespace riistudio::frontend
