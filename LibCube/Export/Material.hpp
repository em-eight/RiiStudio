#pragma once

#include "PropertySupport.hpp"
#include <Lib3D/interface/i3dmodel.hpp>
#include <LibCube/GX/Material.hpp>
#include <LibCore/common.h>

#include <LibCore/api/Node.hpp>

#include "Texture.hpp"

namespace libcube {

// Assumption: all elements are contiguous--no holes
// Much faster than a vector for the many static sized arrays in materials
template<typename T, size_t N>
struct array_vector : public std::array<T, N>
{
	size_t size() const
	{
		return nElements;
	}
	size_t nElements = 0;

	void push_back(T elem)
	{
		at(nElements) = std::move(elem);
		++nElements;
	}
	void pop_back()
	{
		--nElements;
	}
	bool operator==(const array_vector& rhs) const noexcept
	{
		if (rhs.nElements != nElements) return false;
		for (int i = 0; i < nElements; ++i)
		{
			const T& l = this->at(i);
			const T& r = rhs.at(i);
			if (!(l == r))
				return false;
		}
		return true;
	}
};


struct GCMaterialData
{
	std::string name;

	gx::CullMode cullMode;

	// Gen Info counts
	struct GenInfo
	{
		u8 nColorChan = 0;
		u8 nTexGen = 0;
		u8 nTevStage = 1;
		u8 nIndStage = 0;

		bool operator==(const GenInfo& rhs) const
		{
			return nColorChan == rhs.nColorChan &&
				nTexGen == rhs.nTexGen &&
				nTevStage == rhs.nTevStage &&
				nIndStage == rhs.nIndStage;
		}
	};
	GenInfo info;

	struct ChannelData
	{
		gx::Color matColor;
		gx::Color ambColor;

		bool operator==(const ChannelData& rhs) const
		{
			return matColor == rhs.matColor && ambColor == rhs.ambColor;
		}
	};
	array_vector<ChannelData, 2> chanData;
	// Color0, Alpha0, Color1, Alpha1
	array_vector<gx::ChannelControl, 4> colorChanControls;


	gx::Shader shader;

	array_vector<gx::TexCoordGen, 8> texGens;

	array_vector<gx::Color, 4> tevKonstColors;
	array_vector<gx::ColorS10, 4> tevColors;

	bool earlyZComparison = true;
	gx::ZMode zMode;

	// Split up -- only 3 indmtx
	std::vector<gx::IndirectTextureScalePair> mIndScales;
	std::vector<gx::IndirectMatrix> mIndMatrices;

	gx::AlphaComparison alphaCompare;
	gx::BlendMode blendMode;
	bool dither;
	enum class CommonMappingOption
	{
		NoSelection,
		DontRemapTextureSpace, // -1 -> 1 (J3D "basic")
		KeepTranslation // Don't reset translation column
	};

	enum class CommonMappingMethod
	{
		// Shared
		Standard,
		EnvironmentMapping,

		// J3D name. This is G3D's only PROJMAP.
		ViewProjectionMapping,

		// J3D only by default. EGG adds this to G3D as "ManualProjectionMapping"
		ProjectionMapping,
		ManualProjectionMapping = ProjectionMapping,

		// G3D
		EnvironmentLightMapping,
		EnvironmentSpecularMapping,

		// J3D only?
		ManualEnvironmentMapping // Specify effect matrix maunally
		// J3D 4/5?
	};
	enum class CommonTransformModel
	{
		Default,
		Maya,
		Max,
		XSI
	};
	struct TexMatrix
	{
		gx::TexGenType projection; // Only 3x4 and 2x4 valid

		glm::vec2	scale;
		f32			rotate;
		glm::vec2	translate;

		std::array<f32, 16>	effectMatrix;

		CommonTransformModel transformModel;
		CommonMappingMethod method;
		CommonMappingOption option;

		virtual glm::mat3x4 compute(const glm::mat4& mdl, const glm::mat4& mvp);
		// TODO: Support / restriction

		virtual bool operator==(const TexMatrix& rhs) const
		{
			return projection == rhs.projection && scale == rhs.scale && rotate == rhs.rotate && translate == rhs.translate &&
				effectMatrix == rhs.effectMatrix && transformModel == rhs.transformModel && method == rhs.method &&
				option == rhs.option;
		}

		virtual ~TexMatrix() = default;
	};

	array_vector<std::unique_ptr<TexMatrix>, 10> texMatrices;

	struct SamplerData
	{
		std::string mTexture;
		std::string mPalette;

		gx::TextureWrapMode mWrapU = gx::TextureWrapMode::Repeat;
		gx::TextureWrapMode mWrapV = gx::TextureWrapMode::Repeat;

		bool bMipMap = false;
		bool bEdgeLod;
		bool bBiasClamp;

		u8 mMaxAniso;
		u8 mMinFilter;
		u8 mMagFilter;
		s16 mLodBias;

		bool operator==(const SamplerData& rhs) const noexcept
		{
			return mTexture == rhs.mTexture && mWrapU == rhs.mWrapU && mWrapV == rhs.mWrapV &&
				bMipMap == rhs.bMipMap && bEdgeLod == rhs.bEdgeLod && bBiasClamp == rhs.bBiasClamp &&
				mMaxAniso == rhs.mMaxAniso && mMinFilter == rhs.mMinFilter && mMagFilter == rhs.mMagFilter &&
				mLodBias == rhs.mLodBias;
		}
	};

	array_vector<std::unique_ptr<SamplerData>, 8> samplers;
};

struct IGCMaterial : public lib3d::Material
{
    PX_TYPE_INFO("GC Material", "gc_mat", "GC::IMaterialDelegate");
    
    enum class Feature
    {
        CullMode,
        ZCompareLoc,
        ZCompare,
        GenInfo,

        MatAmbColor,

        Max
    };
    // Compat
    struct PropertySupport : public TPropertySupport<Feature>
    {
        using Feature = Feature;
        static constexpr std::array<const char*, (u64)Feature::Max> featureStrings = {
            "Culling Mode",
            "Early Z Comparison",
            "Z Comparison",
            "GenInfo",
            "Material/Ambient Colors"
        };
    };
    
    PropertySupport support;

	virtual GCMaterialData& getMaterialData() = 0;
	virtual const GCMaterialData& getMaterialData() const = 0;

	
	std::pair<std::string, std::string> generateShaders() const override;
	void generateUniforms(DelegatedUBOBuilder& builder,
		const glm::mat4& M, const glm::mat4& V, const glm::mat4& P, u32 shaderId, const std::map<std::string, u32>& texIdMap) const override;

	virtual const Texture& getTexture(const std::string& id) const = 0;
	void genSamplUniforms(u32 shaderId, const std::map<std::string, u32>& texIdMap) const override;


	std::string getName() const override { return getMaterialData().name; }
	void setName(const std::string& name) override { getMaterialData().name = name; }

	void setMegaState(MegaState& state) const override;

	void configure(lib3d::PixelOcclusion occlusion, std::vector<std::string>& textures) override
	{
		// TODO
		if (textures.empty()) return;

		const auto tex = textures[0];

		auto& mat = getMaterialData();

		auto sampler = std::make_unique<GCMaterialData::SamplerData>();
		sampler->mTexture = tex;
		// TODO: EdgeLod/BiasClamp
		// TODO: MaxAniso, filter, bias
		mat.samplers.push_back(std::move(sampler));
		mat.samplers.nElements = 1;

		mat.texGens.push_back(gx::TexCoordGen{ gx::TexGenType::Matrix3x4, gx::TexGenSrc::UV0, gx::TexMatrix::TexMatrix0, false, gx::PostTexMatrix::Identity });
		mat.texGens.nElements = 1;
		auto mtx = std::make_unique<GCMaterialData::TexMatrix>();
		mtx->projection = gx::TexGenType::Matrix3x4;
		mtx->scale = { 1, 1 };
		mtx->rotate = 0;
		mtx->translate = { 0, 0 };
		mtx->effectMatrix = { 0 };
		mtx->transformModel = GCMaterialData::CommonTransformModel::Maya;
		mtx->method = GCMaterialData::CommonMappingMethod::Standard;
		mtx->option = GCMaterialData::CommonMappingOption::NoSelection;

		mat.texMatrices.push_back(std::move(mtx));
		mat.texMatrices.nElements = 1;

		mat.shader.mStages[0].texMap = 0;
		mat.shader.mStages[0].texCoord = 0;
		mat.shader.mStages[0].colorStage.d = gx::TevColorArg::texc;
		mat.shader.mStages[0].alphaStage.d = gx::TevAlphaArg::texa;
	}
};

}
