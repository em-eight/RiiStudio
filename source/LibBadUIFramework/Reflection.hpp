#pragma once

#include "RichName.hpp"
#include <core/common.h>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <string_view>

namespace kpi {

// Not the most clean reflection implementation but it meets all of the minimum
// requirements.
//
struct MirrorEntry {
  const std::string_view derived;
  const std::string_view base;

  // The cast part. We'll need to add more for scripting languages.
  int translation;

  MirrorEntry(const std::string_view d, const std::string_view b, int t)
      : derived(d), base(b), translation(t) {}
};

template <typename D, typename B> constexpr int computeTranslation() {
  return static_cast<int>(reinterpret_cast<char*>(static_cast<B*>(
                              reinterpret_cast<D*>(0x10000000))) -
                          reinterpret_cast<char*>(0x10000000));
}

struct InternalClassMirror {
  std::string derived;
  kpi::RichName mName;

  struct Entry {
    const std::string parent;
    int translation;

    void* cast(void* base) const {
      return reinterpret_cast<char*>(base) + translation;
    }
  };
  std::vector<Entry> mParents;
  std::vector<std::string> mChildren;
};

struct DataMesh {
  virtual ~DataMesh() = default;
  virtual kpi::InternalClassMirror* get(const std::string& id) = 0;
  virtual void declare(const std::string& id, kpi::RichName name) = 0;
  virtual void enqueueHierarchy(kpi::MirrorEntry entry) = 0;
  virtual void compute() = 0;
};

class ReflectionMesh {
  static ReflectionMesh sInstance;

public:
  ReflectionMesh();
  virtual ~ReflectionMesh() = default;

  static inline ReflectionMesh* getInstance() { return &sInstance; }

  inline DataMesh& getDataMesh() { return *mDataMesh.get(); }

  class ReflectionInfoHandle {
  public:
    bool valid() const { return mMirror != nullptr && mMesh != nullptr; }
    operator bool() const { return valid(); }

    std::string getName() const {
      if (!valid())
        return "";
      return mMirror->derived;
    }
    size_t getNumParents() const {
      if (!valid())
        return 0;
      return mMirror->mParents.size();
    }
    ReflectionInfoHandle getParent(size_t index) const {
      if (!valid() || index > std::size(mMirror->mParents))
        return {};
      return ReflectionInfoHandle(mMesh, mMirror->mParents[index].parent);
    }
    size_t getNumChildren() const {
      if (!valid())
        return 0;
      return mMirror->mChildren.size();
    }
    ReflectionInfoHandle getChild(size_t index) const {
      if (!valid() || index > mMirror->mChildren.size())
        return {};
      return ReflectionInfoHandle(mMesh, mMirror->mChildren[index]);
    }
    int getTranslationForParent(size_t index) const {
      // TODO
      assert(valid() && index <= mMirror->mParents.size());

      return mMirror->mParents[index].translation;
    }
    void* castToImmediateParentRaw(void* in, int index) {
      char* p = reinterpret_cast<char*>(in);
      p += getTranslationForParent(index);
      return p;
    }
    template <typename T> T* castToImmediateParent(T* in, int index) {
      assert(T::TypeInfo.namespacedId == getParent(index).getName());

      return reinterpret_cast<T*>(castToImmediateParentRaw(in, index));
    }

  public:
    ReflectionInfoHandle(DataMesh* mesh, const std::string& id) {
      auto* info = mesh->get(id);
      if (info == nullptr) {
        // DebugReport("Type %s has no info...\n", id.c_str());
        return;
      }

      mMirror = info;
      mMesh = mesh;
    }

  private:
    DataMesh* mMesh = nullptr;
    InternalClassMirror* mMirror = nullptr;
    ReflectionInfoHandle(InternalClassMirror* mirror = nullptr,
                         DataMesh* mesh = nullptr)
        : mMesh(mesh), mMirror(mirror) {}
  };

  virtual ReflectionInfoHandle lookupInfo(std::string info);
  virtual void findParentOfType(std::vector<void*>& out, void* in,
                                const std::string& info,
                                const std::string& key);

  template <typename TDst, typename TDynamic>
  inline std::vector<TDst*> findParentOfType(TDynamic&& type) {
    std::vector<void*> out;
    findParentOfType(out, type.mBase, std::string(type.mType),
                     std::string(TDst::TypeInfo.namespacedId));

    std::vector<TDst*> dst;
    for (void* e : out)
      dst.emplace_back(reinterpret_cast<TDst*>(e));
    return dst;
  }

private:
  std::unique_ptr<DataMesh> mDataMesh;
};

} // namespace kpi
