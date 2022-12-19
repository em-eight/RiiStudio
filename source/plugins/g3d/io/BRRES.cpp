#include <core/common.h>
#include <core/kpi/Node2.hpp>
#include <core/kpi/Plugins.hpp>

#include <oishii/reader/binary_reader.hxx>
#include <oishii/writer/binary_writer.hxx>

#include <librii/g3d/io/DictWriteIO.hpp>
#include <plugins/g3d/collection.hpp>

#include <set>
#include <string>

#include <librii/g3d/io/AnimIO.hpp>
#include <librii/g3d/io/ArchiveIO.hpp>

#include <rsl/Ranges.hpp>

namespace librii::g3d {
using namespace bad;
}

namespace riistudio::g3d {

#pragma region Bones

u32 computeFlag(const librii::g3d::BoneData& data,
                std::span<const librii::g3d::BoneData> all,
                bool display_matrix) {
  u32 flag = 0;
  const std::array<float, 3> scale{data.mScaling.x, data.mScaling.y,
                                   data.mScaling.z};
  if (rsl::RangeIsHomogenous(scale)) {
    flag |= 0x10;
    if (data.mScaling == glm::vec3{1.0f, 1.0f, 1.0f})
      flag |= 8;
  }
  if (data.mRotation == glm::vec3{0.0f, 0.0f, 0.0f})
    flag |= 4;
  if (data.mTranslation == glm::vec3{0.0f, 0.0f, 0.0f})
    flag |= 2;
  if ((flag & (2 | 4 | 8)) == (2 | 4 | 8))
    flag |= 1;
  bool has_ssc_below = false;
  {
    std::vector<s32> stack;
    for (auto c : data.mChildren)
      stack.push_back(c);
    while (!stack.empty()) {
      auto& elem = all[stack.back()];
      stack.resize(stack.size() - 1);
      if (elem.ssc) {
        has_ssc_below = true;
      }
      // It seems it's not actually a recursive flag?

      // for (auto c : elem.mChildren)
      //   stack.push_back(c);
    }
    if (has_ssc_below) {
      flag |= 0x40;
    }
  }
  if (data.ssc)
    flag |= 0x20;
  if (!data.classicScale)
    flag |= 0x80;
  if (data.visible)
    flag |= 0x100;
  // TODO: Check children?
  if (!data.mDisplayCommands.empty() /* TODO: Might need to check if any display
                                        command has currentMtx set */
      || display_matrix)
    flag |= 0x200;
  // TODO: 0x400 check parents
  return flag;
}
// Call this last
void setFromFlag(librii::g3d::BoneData& data, u32 flag) {
  // TODO: Validate items
  data.ssc = (flag & 0x20) != 0;
  data.classicScale = (flag & 0x80) == 0;
  data.visible = (flag & 0x100) != 0;
}

librii::g3d::BoneData fromBinaryBone(const librii::g3d::BinaryBoneData& bin) {
  librii::g3d::BoneData bone;
  bone.mName = bin.name;
  bone.matrixId = bin.matrixId;
  bone.billboardType = bin.billboardType;
  // TODO: refId
  bone.mScaling = bin.scale;
  bone.mRotation = bin.rotate;
  bone.mTranslation = bin.translate;

  bone.mVolume = bin.aabb;

  bone.mParent = bin.parent_id;
  // Skip sibling and child links -- we recompute it all
  bone.modelMtx = bin.modelMtx;
  bone.inverseModelMtx = bin.inverseModelMtx;

  setFromFlag(bone, bin.flag);
  return bone;
}

librii::g3d::BinaryBoneData
toBinaryBone(const librii::g3d::BoneData& bone,
             std::span<const librii::g3d::BoneData> bones, u32 bone_id,
             const std::set<s16>& displayMatrices) {
  librii::g3d::BinaryBoneData bin;
  bin.name = bone.mName;
  bin.matrixId = bone.matrixId;

  // TODO: Fix
  bool is_display = displayMatrices.contains(bone.matrixId) || true;
  bin.flag = computeFlag(bone, bones, is_display);
  bin.id = bone_id;

  bin.billboardType = bone.billboardType;
  bin.ancestorBillboardBone = 0; // TODO: ref
  bin.scale = bone.mScaling;
  bin.rotate = bone.mRotation;
  bin.translate = bone.mTranslation;
  bin.aabb = bone.mVolume;

  // Parent, Child, Left, Right
  bin.parent_id = bone.mParent;
  bin.child_first_id = -1;
  bin.sibling_left_id = -1;
  bin.sibling_right_id = -1;

  if (bone.mChildren.size()) {
    bin.child_first_id = bone.mChildren[0];
  }
  if (bone.mParent != -1) {
    auto& siblings = bones[bone.mParent].mChildren;
    auto it = std::find(siblings.begin(), siblings.end(), bone_id);
    assert(it != siblings.end());
    // Not a cyclic linked list
    bin.sibling_left_id = it == siblings.begin() ? -1 : *(it - 1);
    bin.sibling_right_id = it == siblings.end() - 1 ? -1 : *(it + 1);
  }
  bin.modelMtx = bone.modelMtx;
  bin.inverseModelMtx = bone.inverseModelMtx;
  return bin;
}

#pragma endregion

#pragma region librii->Editor

// Handles all incoming bytecode on a method and applies it to the model.
//
// clang-format off
//
// - DrawOpa(material, bone, mesh) -> assert(material.xlu) && bone.addDrawCall(material, mesh)
// - DrawXlu(material, bone, mesh) -> assert(!material.xlu) && bone.addDrawCall(material, mesh)
// - EvpMtx(mtxId, boneId) -> model.insertDrawMatrix(mtxId, { .bone = boneId, .weight = 100% })
// - NodeMix(mtxId, [(mtxId, ratio)]) -> model.insertDrawMatrix(mtxId, ... { .bone = LUT[mtxId], .ratio = ratio })
//
// clang-format on
class ByteCodeHelper {
  using B = librii::g3d::ByteCodeLists;

public:
  ByteCodeHelper(librii::g3d::ByteCodeMethod& method_,
                 riistudio::g3d::Model& mdl_, librii::g3d::BinaryModel& bmdl_,
                 kpi::IOContext& ctx_)
      : method(method_), mdl(mdl_), binary_mdl(bmdl_), ctx(ctx_) {}

  void onDraw(const B::Draw& draw) {
    Bone::Display disp{
        .matId = static_cast<u32>(draw.matId),
        .polyId = static_cast<u32>(draw.polyId),
        .prio = draw.prio,
    };
    auto boneIdx = draw.boneId;
    if (boneIdx > mdl.getBones().size()) {
      ctx.error("Invalid bone index in render command");
      boneIdx = 0;
      ctx.transaction.state = kpi::TransactionState::Failure;
    }

    if (disp.matId > mdl.getMeshes().size()) {
      ctx.error("Invalid material index in render command");
      disp.matId = 0;
      ctx.transaction.state = kpi::TransactionState::Failure;
    }

    if (disp.polyId > mdl.getMeshes().size()) {
      ctx.error("Invalid mesh index in render command");
      disp.polyId = 0;
      ctx.transaction.state = kpi::TransactionState::Failure;
    }

    mdl.getBones()[boneIdx].addDisplay(disp);

    // While with this setup, materials could be XLU and OPA, in
    // practice, they're not.
    //
    // Warn the user if a material is flagged as OPA/XLU but doesn't exist
    // in the corresponding draw list.
    auto& mat = mdl.getMaterials()[disp.matId];
    auto& poly = mdl.getMeshes()[disp.polyId];
    {
      const bool xlu_mat = mat.flag & 0x80000000;
      if ((method.name == "DrawOpa" && xlu_mat) ||
          (method.name == "DrawXlu" && !xlu_mat)) {
        kpi::IOContext mc = ctx.sublet("materials").sublet(mat.name);
        mc.request(
            false,
            "Material {} (#{}) is rendered in the {} pass (with mesh {} #{}), "
            "but is marked as {}",
            mat.name, disp.matId, xlu_mat ? "Opaque" : "Translucent",
            disp.polyId, poly.mName, !xlu_mat ? "Opaque" : "Translucent");
      }
    }
    // And ultimately reset the flag to its proper value.
    mat.xlu = method.name == "DrawXlu";
  }

  void onNodeDesc(const B::NodeDescendence& desc) {
    auto& bone = mdl.getBones()[desc.boneId];
    const auto matrixId = bone.matrixId;

    auto parent_id =
        binary_mdl.info.mtxToBoneLUT.mtxIdToBoneId[desc.parentMtxId];
    if (bone.mParent != -1 && parent_id >= 0) {
      bone.mParent = parent_id;
    }

    if (matrixId >= mdl.mDrawMatrices.size()) {
      mdl.mDrawMatrices.resize(matrixId + 1);
      mdl.mDrawMatrices[matrixId].mWeights.emplace_back(desc.boneId, 1.0f);
    }
  }

  // Either-or: A matrix is either single-bound (EVP) or multi-influence
  // (NODEMIX)
  void onEvpMtx(const B::EnvelopeMatrix& evp) {
    auto& drw = insertMatrix(evp.mtxId);
    drw.mWeights = {{evp.nodeId, 1.0f}};
  }

  void onNodeMix(const B::NodeMix& mix) {
    auto& drw = insertMatrix(mix.mtxId);
    auto range =
        mix.blendMatrices |
        std::views::transform([&](const B::NodeMix::BlendMtx& blend)
                                  -> libcube::DrawMatrix::MatrixWeight {
          int boneIndex =
              binary_mdl.info.mtxToBoneLUT.mtxIdToBoneId[blend.mtxId];
          assert(boneIndex != -1);
          return {static_cast<u32>(boneIndex), blend.ratio};
        });
    drw.mWeights = {range.begin(), range.end()};
  }

private:
  libcube::DrawMatrix& insertMatrix(std::size_t index) {
    if (mdl.mDrawMatrices.size() <= index) {
      mdl.mDrawMatrices.resize(index + 1);
    }
    return mdl.mDrawMatrices[index];
  }
  librii::g3d::ByteCodeMethod& method;
  riistudio::g3d::Model& mdl;
  librii::g3d::BinaryModel& binary_mdl;
  kpi::IOContext& ctx;
};

void processModel(librii::g3d::BinaryModel& binary_model,
                  kpi::LightIOTransaction& transaction,
                  const std::string& transaction_path,
                  riistudio::g3d::Model& mdl) {
  using namespace librii::g3d;
  if (transaction.state == kpi::TransactionState::Failure) {
    return;
  }
  kpi::IOContext ctx(transaction_path + "//MDL0 " + binary_model.name,
                     transaction);

  mdl.mName = binary_model.name;
  // Assign info
  {
    const auto& info = binary_model.info;
    mdl.mScalingRule = info.scalingRule;
    mdl.mTexMtxMode = info.texMtxMode;
    // Validate numVerts, numTris
    {
      auto [computedNumVerts, computedNumTris] =
          librii::gx::computeVertTriCounts(binary_model.meshes);
      ctx.request(
          computedNumVerts == info.numVerts,
          "Model header specifies {} vertices, but the file only has {}.",
          info.numVerts, computedNumVerts);
      ctx.request(
          computedNumTris == info.numTris,
          "Model header specifies {} triangles, but the file only has {}.",
          info.numTris, computedNumTris);
    }
    mdl.sourceLocation = info.sourceLocation;
    {
      auto displayMatrices =
          librii::gx::computeDisplayMatricesSubset(binary_model.meshes);
      ctx.request(info.numViewMtx == displayMatrices.size(),
                  "Model header specifies {} display matrices, but the mesh "
                  "data only references {} display matrices.",
                  info.numViewMtx, displayMatrices.size());
    }
    {
      auto needsNormalMtx =
          std::any_of(binary_model.meshes.begin(), binary_model.meshes.end(),
                      [](auto& m) { return m.needsNormalMtx(); });
      ctx.request(
          info.normalMtxArray == needsNormalMtx,
          needsNormalMtx
              ? "Model header unecessarily burdens the runtime library "
                "with bone-normal-matrix computation"
              : "Model header does not tell the runtime library to maintain "
                "bone normal matrix arrays, although some meshes need it");
    }
    {
      auto needsTexMtx =
          std::any_of(binary_model.meshes.begin(), binary_model.meshes.end(),
                      [](auto& m) { return m.needsTextureMtx(); });
      ctx.request(
          info.texMtxArray == needsTexMtx,
          needsTexMtx
              ? "Model header unecessarily burdens the runtime library "
                "with bone-texture-matrix computation"
              : "Model header does not tell the runtime library to maintain "
                "bone texture matrix arrays, although some meshes need it");
    }
    ctx.request(!info.boundVolume,
                "Model specifies bounding data should be used");
    mdl.mEvpMtxMode = info.evpMtxMode;
    mdl.aabb.min = info.min;
    mdl.aabb.max = info.max;
    // Validate mtxToBoneLUT
    {
      const auto& lut = info.mtxToBoneLUT.mtxIdToBoneId;
      for (size_t i = 0; i < binary_model.bones.size(); ++i) {
        const auto& bone = binary_model.bones[i];
        if (bone.matrixId > lut.size()) {
          ctx.error(
              std::format("Bone {} specifies a matrix ID of {}, but the matrix "
                          "LUT only specifies {} matrices total.",
                          bone.name, bone.matrixId, lut.size()));
          continue;
        }
        ctx.request(lut[bone.matrixId] == i,
                    "Bone {} (#{}) declares ownership of Matrix{}. However, "
                    "Matrix{} does not register this bone as its owner. "
                    "Rather, it specifies an owner ID of {}.",
                    bone.name, i, bone.matrixId, bone.matrixId,
                    lut[bone.matrixId]);
      }
    }
  }

  mdl.getBones().resize(binary_model.bones.size());
  for (size_t i = 0; i < binary_model.bones.size(); ++i) {
    static_cast<librii::g3d::BoneData&>(mdl.getBones()[i]) =
        fromBinaryBone(binary_model.bones[i]);
  }

  // Compute children
  for (int i = 0; i < mdl.getBones().size(); ++i) {
    if (const auto parent_id = mdl.getBones()[i].mParent; parent_id >= 0) {
      if (parent_id >= mdl.getBones().size()) {
        printf("Invalidly large parent index..\n");
        break;
      }
      mdl.getBones()[parent_id].mChildren.push_back(i);
    }
  }

  for (auto& pos : binary_model.positions) {
    static_cast<librii::g3d::PositionBuffer&>(mdl.getBuf_Pos().add()) = pos;
  }
  for (auto& norm : binary_model.normals) {
    static_cast<librii::g3d::NormalBuffer&>(mdl.getBuf_Nrm().add()) = norm;
  }
  for (auto& color : binary_model.colors) {
    static_cast<librii::g3d::ColorBuffer&>(mdl.getBuf_Clr().add()) = color;
  }
  for (auto& texcoord : binary_model.texcoords) {
    static_cast<librii::g3d::TextureCoordinateBuffer&>(mdl.getBuf_Uv().add()) =
        texcoord;
  }

  // TODO: Fura

  for (auto& mat : binary_model.materials) {
    static_cast<librii::g3d::G3dMaterialData&>(mdl.getMaterials().add()) = mat;
  }
  for (auto& mesh : binary_model.meshes) {
    static_cast<librii::g3d::PolygonData&>(mdl.getMeshes().add()) = mesh;
  }

  // Process bytecode: Apply to materials/bones/draw matrices

  for (auto& method : binary_model.bytecodes) {
    ByteCodeHelper helper(method, mdl, binary_model, ctx);
    for (auto& command : method.commands) {
      if (auto* draw = std::get_if<ByteCodeLists::Draw>(&command)) {
        helper.onDraw(*draw);
      } else if (auto* desc =
                     std::get_if<ByteCodeLists::NodeDescendence>(&command)) {
        helper.onNodeDesc(*desc);
      } else if (auto* evp =
                     std::get_if<ByteCodeLists::EnvelopeMatrix>(&command)) {
        helper.onEvpMtx(*evp);
      } else if (auto* mix = std::get_if<ByteCodeLists::NodeMix>(&command)) {
        helper.onNodeMix(*mix);
      }
      // TODO: Other bytecodes
    }
  }

  // Recompute parent-child relationships
  for (size_t i = 0; i < mdl.getBones().size(); ++i) {
    auto& bone = mdl.getBones()[i];
    if (bone.mParent == -1) {
      continue;
    }
    auto& parent = mdl.getBones()[bone.mParent];
    parent.mChildren.push_back(i);
  }
}

#pragma endregion

#pragma region Editor->librii

void BuildRenderLists(const Model& mdl,
                      std::vector<librii::g3d::ByteCodeMethod>& renderLists) {
  librii::g3d::ByteCodeMethod nodeTree{.name = "NodeTree"};
  librii::g3d::ByteCodeMethod nodeMix{.name = "NodeMix"};
  librii::g3d::ByteCodeMethod drawOpa{.name = "DrawOpa"};
  librii::g3d::ByteCodeMethod drawXlu{.name = "DrawXlu"};

  for (size_t i = 0; i < mdl.getBones().size(); ++i) {
    for (const auto& draw : mdl.getBones()[i].mDisplayCommands) {
      librii::g3d::ByteCodeLists::Draw cmd{
          .matId = static_cast<u16>(draw.mMaterial),
          .polyId = static_cast<u16>(draw.mPoly),
          .boneId = static_cast<u16>(i),
          .prio = draw.mPrio,
      };
      bool xlu = draw.mMaterial < std::size(mdl.getMaterials()) &&
                 mdl.getMaterials()[draw.mMaterial].xlu;
      (xlu ? &drawXlu : &drawOpa)->commands.push_back(cmd);
    }
    auto parent = mdl.getBones()[i].mParent;
    assert(parent < std::ssize(mdl.getBones()));
    librii::g3d::ByteCodeLists::NodeDescendence desc{
        .boneId = static_cast<u16>(i),
        .parentMtxId =
            static_cast<u16>(parent >= 0 ? mdl.getBones()[parent].matrixId : 0),
    };
    nodeTree.commands.push_back(desc);
  }

  auto write_drw = [&](const libcube::DrawMatrix& drw, size_t i) {
    if (drw.mWeights.size() > 1) {
      librii::g3d::ByteCodeLists::NodeMix mix{
          .mtxId = static_cast<u16>(i),
      };
      for (auto& weight : drw.mWeights) {
        assert(weight.boneId < mdl.getBones().size());
        mix.blendMatrices.push_back(
            librii::g3d::ByteCodeLists::NodeMix::BlendMtx{
                .mtxId =
                    static_cast<u16>(mdl.getBones()[weight.boneId].matrixId),
                .ratio = weight.weight,
            });
      }
      nodeMix.commands.push_back(mix);
    } else {
      assert(drw.mWeights[0].boneId < mdl.getBones().size());
      librii::g3d::ByteCodeLists::EnvelopeMatrix evp{
          .mtxId = static_cast<u16>(i),
          .nodeId = static_cast<u16>(drw.mWeights[0].boneId),
      };
      nodeMix.commands.push_back(evp);
    }
  };

  // TODO: Better heuristic. When do we *need* NodeMix? Presumably when at least
  // one bone is weighted to a matrix that is not a bone directly? Or when that
  // bone is influenced by another bone?
  bool needs_nodemix = false;
  for (auto& mtx : mdl.mDrawMatrices) {
    if (mtx.mWeights.size() > 1) {
      needs_nodemix = true;
      break;
    }
  }

  if (needs_nodemix) {
    // Bones come first
    for (auto& bone : mdl.getBones()) {
      auto& drw = mdl.mDrawMatrices[bone.matrixId];
      write_drw(drw, bone.matrixId);
    }
    for (size_t i = 0; i < mdl.mDrawMatrices.size(); ++i) {
      auto& drw = mdl.mDrawMatrices[i];
      if (drw.mWeights.size() == 1) {
        // Written in pre-pass
        continue;
      }
      write_drw(drw, i);
    }
  }

  renderLists.push_back(nodeTree);
  if (!nodeMix.commands.empty()) {
    renderLists.push_back(nodeMix);
  }
  if (!drawOpa.commands.empty()) {
    renderLists.push_back(drawOpa);
  }
  if (!drawXlu.commands.empty()) {
    renderLists.push_back(drawXlu);
  }
}
librii::g3d::BinaryModel toBinaryModel(const Model& mdl) {
  std::set<s16> displayMatrices =
      librii::gx::computeDisplayMatricesSubset(mdl.getMeshes());
  auto bones = mdl.getBones() | rsl::ToList<librii::g3d::BoneData>();
  auto to_binary_bone = [&](auto tuple) {
    auto [index, value] = tuple;
    return toBinaryBone(value, bones, index, displayMatrices);
  };
  librii::g3d::BinaryModel bin{
      .name = mdl.mName,
      .bones = mdl.getBones()     // Start with the bones
               | rsl::enumerate() // Convert to [i, bone] tuples
               | std::views::transform(to_binary_bone) // Convert to BinaryBone
               | rsl::ToList<>(),                      // And back to vector
      .positions =
          mdl.getBuf_Pos() | rsl::ToList<librii::g3d::PositionBuffer>(),
      .normals = mdl.getBuf_Nrm() | rsl::ToList<librii::g3d::NormalBuffer>(),
      .colors = mdl.getBuf_Clr() | rsl::ToList<librii::g3d::ColorBuffer>(),
      .texcoords =
          mdl.getBuf_Uv() | rsl::ToList<librii::g3d::TextureCoordinateBuffer>(),
      .materials =
          mdl.getMaterials() | rsl::ToList<librii::g3d::G3dMaterialData>(),
      .meshes = mdl.getMeshes() | rsl::ToList<librii::g3d::PolygonData>(),
  };

  // Compute ModelInfo
  {
    const auto [nVert, nTri] = computeVertTriCounts(mdl);
    bool nrmMtx = std::ranges::any_of(
        mdl.getMeshes(), [](auto& m) { return m.needsNormalMtx(); });
    bool texMtx = std::ranges::any_of(
        mdl.getMeshes(), [](auto& m) { return m.needsTextureMtx(); });
    librii::g3d::BinaryModelInfo info{
        .scalingRule = mdl.mScalingRule,
        .texMtxMode = mdl.mTexMtxMode,
        .numVerts = nVert,
        .numTris = nTri,
        .sourceLocation = mdl.sourceLocation,
        .numViewMtx = static_cast<u32>(displayMatrices.size()),
        .normalMtxArray = nrmMtx,
        .texMtxArray = texMtx,
        .boundVolume = false,
        .evpMtxMode = mdl.mEvpMtxMode,
        .min = mdl.aabb.min,
        .max = mdl.aabb.max,
    };
    {
      // Matrix -> Bone LUT
      auto& lut = info.mtxToBoneLUT.mtxIdToBoneId;
      lut.resize(mdl.mDrawMatrices.size());
      std::ranges::fill(lut, -1);
      for (const auto& [i, bone] : rsl::enumerate(bin.bones)) {
        lut[bone.matrixId] = i;
      }
    }
    bin.info = info;
  }
  BuildRenderLists(mdl, bin.bytecodes);

  return bin;
}

#pragma endregion

void ReadBRRES(Collection& collection, oishii::BinaryReader& reader,
               kpi::LightIOTransaction& transaction) {
  librii::g3d::BinaryArchive archive;
  archive.read(reader, transaction);
  collection.path = reader.getFile();
  for (auto& mdl : archive.models) {
    auto& editor_mdl = collection.getModels().add();
    processModel(mdl, transaction, "MDL0 " + mdl.name, editor_mdl);
  }
  for (auto& tex : archive.textures) {
    static_cast<librii::g3d::TextureData&>(collection.getTextures().add()) =
        tex;
  }
  for (auto& srt : archive.srts) {
    static_cast<librii::g3d::SrtAnimationArchive&>(
        collection.getAnim_Srts().add()) = srt;
  }
}

void WriteBRRES(Collection& scn, oishii::Writer& writer) {
  librii::g3d::BinaryArchive arc{
      .models = scn.getModels() | std::views::transform(toBinaryModel) |
                rsl::ToList(),
      .textures = scn.getTextures() | rsl::ToList<librii::g3d::TextureData>(),
      .srts =
          scn.getAnim_Srts() | rsl::ToList<librii::g3d::SrtAnimationArchive>(),
  };
  arc.write(writer);
}

} // namespace riistudio::g3d
