#include "AnimIO.hpp"

namespace librii::g3d {

struct SRTOffsets {
  s32 ofsBrres{};
  s32 ofsMatDict{};
  s32 ofsUserData{};

  static constexpr size_t size_bytes() { return 3 * 4; }

  Result<void> read(oishii::BinaryReader& reader) {
    rsl::SafeReader safe(reader);
    ofsBrres = TRY(safe.S32());
    ofsMatDict = TRY(safe.S32());
    ofsUserData = TRY(safe.S32());
    return {};
  }
  void write(oishii::Writer& writer) {
    writer.write<s32>(ofsBrres);
    writer.write<s32>(ofsMatDict);
    writer.write<s32>(ofsUserData);
  }
};

struct BinarySrtInfo {
  std::string name;
  std::string sourcePath;
  u16 frameDuration{};
  u16 materialCount{};
  u32 xformModel{};
  AnimationWrapMode wrapMode{AnimationWrapMode::Repeat};

  Result<void> read(oishii::BinaryReader& reader, u32 pat0) {
    rsl::SafeReader safe(reader);
    name = TRY(safe.StringOfs32(pat0));
    sourcePath = TRY(safe.StringOfs32(pat0));
    frameDuration = TRY(safe.U16());
    materialCount = TRY(safe.U16());
    xformModel = TRY(safe.U32());
    wrapMode = TRY(safe.Enum32<AnimationWrapMode>());
    return {};
  }
  void write(oishii::Writer& writer, NameTable& names, u32 pat0) const {
    writeNameForward(names, writer, pat0, name, true);
    writeNameForward(names, writer, pat0, sourcePath, true);
    writer.write<u16>(frameDuration);
    writer.write<u16>(materialCount);
    writer.write<u32>(xformModel);
    writer.write<u32>(static_cast<u32>(wrapMode));
  }
};

Result<void> BinarySrt::read(oishii::BinaryReader& reader) {
  rsl::SafeReader safe(reader);
  auto srt0 = safe.scoped("SRT0");
  TRY(safe.Magic("SRT0"));
  TRY(safe.U32()); // size
  auto ver = TRY(safe.U32());
  if (ver != 5) {
    return std::unexpected(std::format(
        "Unsupported SRT0 version {}. Only version 5 is supported.", ver));
  }
  SRTOffsets offsets;
  TRY(offsets.read(reader));

  BinarySrtInfo info;
  TRY(info.read(reader, srt0.start));
  name = info.name;
  sourcePath = info.sourcePath;
  frameDuration = info.frameDuration;
  wrapMode = info.wrapMode;

  auto track_addr_to_index = [&](u32 addr) -> Result<u32> {
    auto back = safe.tell();
    reader.seekSet(addr);
    SRT0Track track;
    TRY(track.read(safe));
    reader.seekSet(back);
    auto it = std::find(tracks.begin(), tracks.end(), track);
    if (it != tracks.end()) {
      return it - tracks.begin();
    }
    tracks.push_back(track);
    return tracks.size() - 1;
  };

  reader.seekSet(srt0.start + offsets.ofsMatDict);
  auto slice = reader.slice();
  if (slice.empty()) {
    return std::unexpected("Unable to read dictionary");
  }
  DictionaryRange matDict(slice, reader.tell(), info.materialCount + 1);

  for (const auto& node : matDict) {
    auto& mat = materials.emplace_back();
    reader.seekSet(node.abs_data_ofs);
    TRY(mat.read(safe, track_addr_to_index));
  }
  return {};
}

void BinarySrt::write(oishii::Writer& writer, NameTable& names,
                      u32 addrBrres) const {
  auto start = writer.tell();
  writer.write<u32>('SRT0');
  writer.write<u32>(0, false);
  writer.write<u32>(5);
  auto wb = writer.tell();
  SRTOffsets offsets;
  offsets.ofsBrres = addrBrres - start;
  writer.skip(offsets.size_bytes());

  BinarySrtInfo info{
      .name = name,
      .sourcePath = sourcePath,
      .frameDuration = frameDuration,
      .materialCount = static_cast<u16>(materials.size()),
      .xformModel = xformModel,
      .wrapMode = wrapMode,
  };
  info.write(writer, names, start);

  BetterDictionary dict;

  std::vector<u32> track_addresses;
  // Edge case: no root node if 1 entry
  auto dictSize = CalcDictionarySize(materials.size());
  u32 accum = start + 0x2C /* header */ + dictSize;
  for (auto& mat : materials) {
    dict.nodes.push_back(BetterNode{.name = mat.name, .stream_pos = accum});
    accum += mat.computeSize();
  }
  for (auto& track : tracks) {
    track_addresses.push_back(accum);
    accum += track.computeSize();
  }
  auto track_index_to_addr = [&](u32 i) { return track_addresses[i]; };

  offsets.ofsMatDict = writer.tell() - start;
  WriteDictionary(dict, writer, names);
  for (auto& mat : materials) {
    mat.write(writer, names, track_index_to_addr);
  }
  for (auto& track : tracks) {
    track.write(writer);
  }

  auto back = writer.tell();
  writer.seekSet(wb);
  offsets.write(writer);
  writer.seekSet(start + 4);
  writer.write<u32>(back - start);
  writer.seekSet(back);
}

u32 SRT0Track::computeSize() const { return 8 + keyframes.size() * 12; }
Result<void> SRT0Track::read(rsl::SafeReader& safe) {
  auto numFrames = TRY(safe.U16());
  for (auto& e : reserved) {
    e = TRY(safe.U8());
  }
  step = TRY(safe.F32());
  for (u32 i = 0; i < numFrames; ++i) {
    f32 frame = TRY(safe.F32());
    f32 value = TRY(safe.F32());
    f32 tangent = TRY(safe.F32());
    keyframes.push_back(SRT0KeyFrame{
        .frame = frame,
        .value = value,
        .tangent = tangent,
    });
  }
  return {};
}
void SRT0Track::write(oishii::Writer& writer) const {
  writer.write<u16>(keyframes.size());
  for (auto e : reserved) {
    writer.write<u8>(e);
  }
  writer.write<f32>(step);
  for (const auto& data : keyframes) {
    writer.write<f32>(data.frame);
    writer.write<f32>(data.value);
    writer.write<f32>(data.tangent);
  }
}
u32 SRT0Matrix::computeSize() const {
  u32 accum = 4;
  for (u32 i = 0; i < targets.size(); ++i) {
    accum += 4;
  }
  return accum;
}
static bool IsSrtAttributeIncluded(u32 flags, SRT0Matrix::TargetId attribute) {
  switch (attribute) {
  case SRT0Matrix::TargetId::ScaleU:
    return (flags & SRT0Matrix::FLAG_SCL_ONE) == 0;
  case SRT0Matrix::TargetId::ScaleV:
    return (flags & SRT0Matrix::FLAG_SCL_ISOTROPIC) == 0;
  case SRT0Matrix::TargetId::Rotate:
    return (flags & SRT0Matrix::FLAG_ROT_ZERO) == 0;
  case SRT0Matrix::TargetId::TransU:
  case SRT0Matrix::TargetId::TransV:
    return (flags & SRT0Matrix::FLAG_TRANS_ZERO) == 0;
  default: // Count
    break;
  }
  return false;
}
Result<void>
SRT0Matrix::read(rsl::SafeReader& safe,
                 std::function<Result<u32>(u32)> trackAddressToIndex) {
  auto matrix = safe.scoped("SRT0Matrix");
  flags = TRY(safe.U32());
  fprintf(stderr, "Flags: %u (0x%x)\n", flags, flags);
  for (u32 i = 0; i < static_cast<u32>(TargetId::Count); ++i) {
    if (0 == (flags & (FLAG_ENABLED << (i * 0)))) {
      continue;
    }
    bool included = IsSrtAttributeIncluded(flags, static_cast<TargetId>(i));
    bool fixed = isFixed(static_cast<TargetId>(i), flags);
    fprintf(stderr, "MatrixId: %s %s [%s]\n",
            magic_enum::enum_name(static_cast<TargetId>(i)).data(),
            fixed ? "FIXED" : "ANIM", included ? "INCLUDED" : "OMITTED");
    if (!included) {
      continue;
    }
    if (fixed) {
      f32 constFrame = TRY(safe.F32());
      targets.emplace_back(SRT0Target{.data = constFrame});
    } else {
      auto base = safe.tell();
      safe.getUnsafe().warnAt("Bruh", base, base + 4);
      auto ofs = TRY(safe.S32());
      fprintf(stderr, "base 0x%x ofs %d\n", (u32)base, (int)ofs);
      // For some reason it's relative to the entry entry in SRT0. In
      // PAT0 it's relative to section start.
      u32 index = TRY(trackAddressToIndex(base + ofs));
      targets.emplace_back(SRT0Target{.data = index});
    }
  }
  return {};
}
void SRT0Matrix::write(oishii::Writer& writer,
                       std::function<u32(u32)> trackIndexToAddress) const {
  writer.write<u32>(flags);
  for (auto& target : targets) {
    if (auto* c = std::get_if<f32>(&target.data)) {
      writer.write<f32>(*c);
    } else if (auto* index = std::get_if<u32>(&target.data)) {
      u32 addr = trackIndexToAddress(*index);
      // Relative to offset itself. See comment above.
      auto ofs = addr - writer.tell();
      writer.write<s32>(ofs);
    } else {
      assert(!"Internal error");
    }
  }
}
u32 SRT0Material::computeSize() const {
  return std::accumulate(matrices.begin(), matrices.end(), static_cast<u32>(12),
                         [&](u32 count, auto& x) -> u32 {
                           return count + x.computeSize() +
                                  4 /* For the offset */;
                         });
}
Result<void>
SRT0Material::read(rsl::SafeReader& reader,
                   std::function<Result<u32>(u32)> trackAddressToIndex) {
  auto start = reader.tell();
  name = TRY(reader.StringOfs(start));
  enabled_texsrts = TRY(reader.U32());
  enabled_indsrts = TRY(reader.U32());
  for (u32 i = 0; i < 8; ++i) {
    if (0 == (enabled_texsrts & (FLAG_ENABLED << (i * 1)))) {
      continue;
    }
    {
      auto ofs = TRY(reader.S32());
      u32 at = start + ofs;
      auto back = reader.tell();
      reader.seekSet(at);
      SRT0Matrix mtx{};
      TRY(mtx.read(reader, trackAddressToIndex));
      matrices.push_back(mtx);
      reader.seekSet(back);
    }
  }
  for (u32 i = 0; i < 33; ++i) {
    if (0 == (enabled_indsrts & (FLAG_ENABLED << (i * 1)))) {
      continue;
    }
    {
      auto ofs = TRY(reader.S32());
      u32 at = start + ofs;
      auto back = reader.tell();
      reader.seekSet(at);
      SRT0Matrix mtx{};
      TRY(mtx.read(reader, trackAddressToIndex));
      matrices.push_back(mtx);
      reader.seekSet(back);
    }
  }
  return {};
}
void SRT0Material::write(oishii::Writer& writer, NameTable& names,
                         std::function<u32(u32)> trackIndexToAddress) const {
  auto start = writer.tell();
  writeNameForward(names, writer, start, name, true);
  writer.write<u32>(enabled_texsrts);
  writer.write<u32>(enabled_indsrts);

  u32 count = std::popcount(enabled_texsrts) + std::popcount(enabled_indsrts);
  assert(count == matrices.size());
  u32 accum = start + 12 + count * 4;
  std::vector<u32> matrixAddrs;
  for (u32 i = 0; i < count; ++i) {
    matrixAddrs.push_back(accum);
    accum += matrices[i].computeSize();
  }

  for (u32 i = 0; i < count; ++i) {
    u32 addr = matrixAddrs[i];
    fprintf(stderr, "Target: %x\n", addr);
    auto ofs = addr - start;
    writer.write<s32>(ofs);
  }
  for (auto& mtx : matrices) {
    mtx.write(writer, trackIndexToAddress);
  }
  assert(writer.tell() == accum);
}
} // namespace librii::g3d
