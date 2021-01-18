#pragma once

#include <core/common.h>
#include <librii/gx.h>
#include <plugins/gc/Export/IndexedPolygon.hpp>
#include <vector>

namespace riistudio::j3d {

class Model;

using MatrixPrimitive = librii::gx::MatrixPrimitive;

struct ShapeData : public librii::gx::MeshData {
  u32 id;
  enum class Mode {
    Normal,
    Billboard_XY,
    Billboard_Y,
    Skinned,

    Max
  };
  Mode mode = Mode::Normal;

  f32 bsphere = 100000.0f;
  lib3d::AABB bbox{{-100000.0f, -100000.0f, -100000.0f},
                   {100000.0f, 100000.0f, 100000.0f}};

  bool operator==(const ShapeData& rhs) const = default;

  bool visible = true; // Editor-only
};
struct Shape : public ShapeData,
               public libcube::IndexedPolygon,
               public virtual kpi::IObject {
  void setId(u32 _id) override { id = _id; }
  virtual const j3d::Model* getParent() const { return nullptr; }

  std::string getName() const { return "Shape " + std::to_string(id); }
  void setName(const std::string& name) override {}

 
  MeshData& getMeshData() override { return *this; }
  const MeshData& getMeshData() const { return *this; }
  lib3d::AABB getBounds() const override { return bbox; }

  glm::vec2 getUv(u64 chan, u64 id) const override;
  glm::vec4 getClr(u64 chan, u64 id) const override;
  glm::vec3 getPos(u64 id) const override;
  glm::vec3 getNrm(u64 id) const override;
  u64 addPos(const glm::vec3& v) override;
  u64 addNrm(const glm::vec3& v) override;
  u64 addClr(u64 chan, const glm::vec4& v) override;
  u64 addUv(u64 chan, const glm::vec2& v) override;


  std::vector<glm::mat4> getPosMtx(u64 mpid) const override;

  bool isVisible() const override { return visible; }
  void init(bool skinned, riistudio::lib3d::AABB* boundingBox) override {
    if (skinned)
      mode = j3d::ShapeData::Mode::Skinned;
    if (boundingBox != nullptr)
      bbox = *boundingBox;
  }
};

} // namespace riistudio::j3d
