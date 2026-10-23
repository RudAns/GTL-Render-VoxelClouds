#include "CLRender.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "SceneRenderTargetParameters.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

// ---------------------------------------------------------------- shaders

BEGIN_SHADER_PARAMETER_STRUCT(FCLCommonParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FVector3f, DomainMinTW)
	SHADER_PARAMETER(FVector3f, DomainM)
	SHADER_PARAMETER(float, VoxelM)
	SHADER_PARAMETER(FVector3f, SunDir)
	SHADER_PARAMETER(FVector3f, SunColor)
	SHADER_PARAMETER(FVector3f, SkyColor)
	SHADER_PARAMETER(FVector3f, HazeColor)
	SHADER_PARAMETER(float, HazeKm)
	SHADER_PARAMETER(FVector3f, WindM)
	SHADER_PARAMETER(uint32, FrameIndex)
	SHADER_PARAMETER(uint32, StepMode)
	SHADER_PARAMETER(uint32, SdfSkip)
	SHADER_PARAMETER(uint32, SdfFormat)
	SHADER_PARAMETER(uint32, MaxSteps)
	SHADER_PARAMETER(uint32, Detail)
	SHADER_PARAMETER(uint32, NoiseMipByDistance)
	SHADER_PARAMETER(uint32, LightMode)
	SHADER_PARAMETER(uint32, MultiScatter)
	SHADER_PARAMETER(uint32, Ambient)
	SHADER_PARAMETER(uint32, AmbientDirectional)
	SHADER_PARAMETER(uint32, Lightning)
	SHADER_PARAMETER(uint32, DebugView)
	SHADER_PARAMETER(float, DensityMul)
	SHADER_PARAMETER(uint32, NumFlashes)
	SHADER_PARAMETER_ARRAY(FVector4f, FlashP, [CLRender::MaxFlashes])
	SHADER_PARAMETER_ARRAY(FVector4f, FlashI, [CLRender::MaxFlashes])
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, Modeling)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, SdfR16)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, SdfBC1)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, SdfBC4)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, NoiseTex)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float2>, LightGrid)
	SHADER_PARAMETER_SAMPLER(SamplerState, LinearClamp)
	SHADER_PARAMETER_SAMPLER(SamplerState, LinearWrap)
END_SHADER_PARAMETER_STRUCT()

class FCLLightGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCLLightGridCS);
	SHADER_USE_PARAMETER_STRUCT(FCLLightGridCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCLCommonParameters, Common)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float2>, RWLightGrid)
		SHADER_PARAMETER(uint32, SliceStart)
		SHADER_PARAMETER(uint32, SliceCount)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};
IMPLEMENT_GLOBAL_SHADER(FCLLightGridCS, "/CloudLab/CloudLab.usf", "LightGridCS", SF_Compute);

class FCLMarchCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCLMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FCLMarchCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCLCommonParameters, Common)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWCloud)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWCloudDepth)
		SHADER_PARAMETER(FUintVector2, LowSize)
		SHADER_PARAMETER(uint32, ResDiv)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, ViewMax)
		SHADER_PARAMETER(float, TStartM)
		SHADER_PARAMETER(float, TEndM)
		SHADER_PARAMETER(uint32, LayerTint)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};
IMPLEMENT_GLOBAL_SHADER(FCLMarchCS, "/CloudLab/CloudLab.usf", "MarchCS", SF_Compute);

class FCLCompositePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCLCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FCLCompositePS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, NearCloud)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, NearDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FarCloud)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FarDepth)
		SHADER_PARAMETER(FUintVector2, NearSize)
		SHADER_PARAMETER(FUintVector2, FarSize)
		SHADER_PARAMETER(uint32, NearDiv)
		SHADER_PARAMETER(uint32, FarDiv)
		SHADER_PARAMETER(uint32, UseNear)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};
IMPLEMENT_GLOBAL_SHADER(FCLCompositePS, "/CloudLab/CloudLab.usf", "CompositePS", SF_Pixel);

// ---------------------------------------------------------------- render-thread state

struct FCLCloudRenderer::FRenderState
{
	struct FSet { FTextureRHIRef Modeling, R16, BC1Safe, BC1Naive, BC4Log; };
	FSet Sets[2];
	FTextureRHIRef Noise, LightGrid;
	FCLRenderFrame Frame;
	ECLRepresentation GridRep = ECLRepresentation::Voxel;
	FVector GridSun = FVector::ZeroVector;
	bool bGridValid = false;
	int32 NextSlice = 0;
	FRenderQueryPoolRHIRef QueryPool;
	struct FTimerSet { FRHIPooledRenderQuery Q[5]; bool bIssued = false; FIntPoint Near = FIntPoint::ZeroValue, Far = FIntPoint::ZeroValue; int32 Slices = 0; };
	FTimerSet Timers[4];
	int32 TimerFrame = 0;
};

namespace
{
	FTextureRHIRef Create3D(FRHICommandListImmediate& RHICmdList, const TCHAR* Name, FIntVector Size, EPixelFormat Format, bool bUAV, int32 Mips = 1)
	{
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(Name, Size.X, Size.Y, Size.Z, Format)
			.SetNumMips(Mips)
			.SetFlags(ETextureCreateFlags::ShaderResource | (bUAV ? ETextureCreateFlags::UAV : ETextureCreateFlags::None))
			.SetInitialState(ERHIAccess::SRVMask);
		return RHICmdList.CreateTexture(Desc);
	}

	// Texel data, or BC block data when the format is block compressed (pitches are then per block row / block slice).
	void Upload3D(FRHICommandListImmediate& RHICmdList, FRHITexture* Tex, int32 Mip, FIntVector Size, const uint8* Data, int32 RowPitch, int32 SlicePitch)
	{
		const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Size.X, Size.Y, Size.Z);
		RHICmdList.UpdateTexture3D(Tex, Mip, Region, RowPitch, SlicePitch, Data);
	}
}

FCLCloudRenderer::FCLCloudRenderer(const FAutoRegister& AutoReg, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoReg, InWorld)
	, RT(MakeUnique<FRenderState>())
{
	FRenderState* S = RT.Get();
	ENQUEUE_RENDER_COMMAND(CLCreate)([S](FRHICommandListImmediate& RHICmdList)
	{
		using namespace CLRender;
		S->LightGrid = Create3D(RHICmdList, TEXT("CLLightGrid"), FIntVector(LX, LY, LZ), PF_G16R16F, true);
		S->QueryPool = RHICreateRenderQueryPool(RQT_AbsoluteTime, 64);
	});
}

FCLCloudRenderer::~FCLCloudRenderer()
{
	FRenderState* S = RT.Release();
	ENQUEUE_RENDER_COMMAND(CLFree)([S](FRHICommandListImmediate&) { delete S; });
}

void FCLCloudRenderer::SetFrame(const FCLRenderFrame& Frame)
{
	FRenderState* S = RT.Get();
	ENQUEUE_RENDER_COMMAND(CLFrame)([S, Frame](FRHICommandListImmediate&) { S->Frame = Frame; });
}

void FCLCloudRenderer::UploadVolumes(ECLRepresentation Rep, const FCLVolumeData& Data)
{
	using namespace CLRender;
	const int32 N = NX * NY * NZ;
	const int32 Blocks = (NX / 4) * (NY / 4) * NZ * 8;
	if (Data.Modeling.Num() != N * 4 || Data.SdfR16.Num() != N || Data.SdfBC1Safe.Num() != Blocks || Data.SdfBC1Naive.Num() != Blocks || Data.SdfBC4Log.Num() != Blocks) return;
	FRenderState* S = RT.Get();
	const int32 Index = (int32)Rep;
	ENQUEUE_RENDER_COMMAND(CLUpload)([S, Index, Data](FRHICommandListImmediate& RHICmdList)
	{
		FRenderState::FSet& Set = S->Sets[Index];
		const FIntVector Size(NX, NY, NZ);
		Set.Modeling = Create3D(RHICmdList, TEXT("CLModeling"), Size, PF_R8G8B8A8, false);
		Upload3D(RHICmdList, Set.Modeling, 0, Size, Data.Modeling.GetData(), NX * 4, NX * NY * 4);
		Set.R16 = Create3D(RHICmdList, TEXT("CLSdfR16"), Size, PF_G16, false);
		Upload3D(RHICmdList, Set.R16, 0, Size, (const uint8*)Data.SdfR16.GetData(), NX * 2, NX * NY * 2);
		const int32 BRow = (NX / 4) * 8, BSlice = BRow * (NY / 4);
		Set.BC1Safe = Create3D(RHICmdList, TEXT("CLSdfBC1"), Size, PF_DXT1, false);
		Upload3D(RHICmdList, Set.BC1Safe, 0, Size, Data.SdfBC1Safe.GetData(), BRow, BSlice);
		Set.BC1Naive = Create3D(RHICmdList, TEXT("CLSdfBC1Naive"), Size, PF_DXT1, false);
		Upload3D(RHICmdList, Set.BC1Naive, 0, Size, Data.SdfBC1Naive.GetData(), BRow, BSlice);
		Set.BC4Log = Create3D(RHICmdList, TEXT("CLSdfBC4"), Size, PF_BC4, false);
		Upload3D(RHICmdList, Set.BC4Log, 0, Size, Data.SdfBC4Log.GetData(), BRow, BSlice);
		S->bGridValid = false;
	});
	bHasData = true;
}

void FCLCloudRenderer::UploadNoise(const TArray<TArray<uint8>>& Mips)
{
	using namespace CLRender;
	if (Mips.Num() == 0 || Mips[0].Num() != NoiseN * NoiseN * NoiseN * 4) return;
	FRenderState* S = RT.Get();
	ENQUEUE_RENDER_COMMAND(CLNoise)([S, Mips](FRHICommandListImmediate& RHICmdList)
	{
		S->Noise = Create3D(RHICmdList, TEXT("CLNoise"), FIntVector(NoiseN, NoiseN, NoiseN), PF_R8G8B8A8, false, Mips.Num());
		for (int32 M = 0; M < Mips.Num(); ++M)
		{
			const int32 W = FMath::Max(1, NoiseN >> M);
			Upload3D(RHICmdList, S->Noise, M, FIntVector(W, W, W), Mips[M].GetData(), W * 4, W * W * 4);
		}
	});
}

FCLGpuTimes FCLCloudRenderer::GetGpuTimes() const
{
	FScopeLock L(&TimesLock);
	return Times;
}

void FCLCloudRenderer::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	using namespace CLRender;
	FRenderState& S = *RT;
	const FCLRenderFrame& F = S.Frame;
	const FRenderState::FSet& Set = S.Sets[(int32)F.Settings.Rep];
	if (!Set.Modeling || !S.Noise || !S.LightGrid) return;

	// read back timers issued a few frames ago
	{
		FRenderState::FTimerSet& Old = S.Timers[(S.TimerFrame + 1) % 4];
		if (Old.bIssued)
		{
			uint64 T[5];
			bool bOk = true;
			for (int32 K = 0; K < 5 && bOk; ++K) bOk = RHIGetRenderQueryResult(Old.Q[K].GetQuery(), T[K], false);
			if (bOk)
			{
				FCLGpuTimes G;
				G.LightMs = (T[1] - T[0]) * 0.001f;
				G.NearMs = (T[2] - T[1]) * 0.001f;
				G.FarMs = (T[3] - T[2]) * 0.001f;
				G.CompositeMs = (T[4] - T[3]) * 0.001f;
				G.NearSize = Old.Near; G.FarSize = Old.Far; G.LightSlices = Old.Slices;
				SetTimes(G);
			}
			for (int32 K = 0; K < 5; ++K) Old.Q[K].ReleaseQuery();
			Old.bIssued = false;
		}
	}

	if (!F.Settings.bEnabled || !Inputs.SceneTextures) return;
	Inputs.Validate();
	const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
	FRDGTextureRef SceneColor = (*Inputs.SceneTextures)->SceneColorTexture;
	FRDGTextureRef SceneDepthTex = (*Inputs.SceneTextures)->SceneDepthTexture;
	if (!SceneColor || !SceneDepthTex) return;

	const FVector PreView = View.ViewMatrices.GetPreViewTranslation();
	FGlobalShaderMap* Map = GetGlobalShaderMap(View.GetFeatureLevel());
	RDG_EVENT_SCOPE(GraphBuilder, "CloudLab");

	FRDGTextureRef RModel = RegisterExternalTexture(GraphBuilder, Set.Modeling, TEXT("CLModeling"));
	FRDGTextureRef RR16 = RegisterExternalTexture(GraphBuilder, Set.R16, TEXT("CLSdfR16"));
	FRDGTextureRef RBC1 = RegisterExternalTexture(GraphBuilder, F.Settings.SdfFormat == ECLSdfFormat::BC1Naive ? Set.BC1Naive : Set.BC1Safe, TEXT("CLSdfBC1"));
	FRDGTextureRef RBC4 = RegisterExternalTexture(GraphBuilder, Set.BC4Log, TEXT("CLSdfBC4"));
	FRDGTextureRef RNoise = RegisterExternalTexture(GraphBuilder, S.Noise, TEXT("CLNoise"));
	FRDGTextureRef RGrid = RegisterExternalTexture(GraphBuilder, S.LightGrid, TEXT("CLLightGrid"));

	auto FillCommon = [&](FCLCommonParameters& C, bool bGridReadable)
	{
		const FCLRenderSettings& R = F.Settings;
		C.View = View.ViewUniformBuffer;
		C.DomainMinTW = FVector3f(F.DomainMin + PreView);
		C.DomainM = FVector3f(NX, NY, NZ) * (F.VoxelCm * 0.01f);
		C.VoxelM = F.VoxelCm * 0.01f;
		C.SunDir = FVector3f(F.SunDir.GetSafeNormal());
		C.SunColor = FVector3f(F.SunColor.R, F.SunColor.G, F.SunColor.B);
		C.SkyColor = FVector3f(F.SkyColor.R, F.SkyColor.G, F.SkyColor.B);
		C.HazeColor = FVector3f(F.HazeColor.R, F.HazeColor.G, F.HazeColor.B);
		C.HazeKm = FMath::Max(F.HazeKm, 0.1f);
		C.WindM = FVector3f(F.WindM);
		C.FrameIndex = F.FrameIndex;
		C.StepMode = (uint32)R.StepMode;
		C.SdfSkip = R.bSdfSkip ? 1u : 0u;
		C.SdfFormat = (uint32)R.SdfFormat;
		C.MaxSteps = (uint32)FMath::Max(R.MaxSteps, 8);
		C.Detail = (uint32)R.Detail;
		C.NoiseMipByDistance = R.bNoiseMipByDistance ? 1u : 0u;
		C.LightMode = (uint32)R.Lighting;
		C.MultiScatter = R.bMultiScatter ? 1u : 0u;
		C.Ambient = R.bAmbient ? 1u : 0u;
		C.AmbientDirectional = R.bAmbientDirectional ? 1u : 0u;
		C.Lightning = R.bLightning ? 1u : 0u;
		C.DebugView = (uint32)R.DebugView;
		C.DensityMul = R.DensityMul;
		C.NumFlashes = FMath::Min(F.Flashes.Num(), MaxFlashes);
		for (int32 I = 0; I < (int32)C.NumFlashes; ++I)
		{
			const FVector P = (F.Flashes[I].Pos - F.DomainMin) * 0.01;
			C.FlashP[I] = FVector4f(FVector3f(P), F.Flashes[I].RadiusM);
			C.FlashI[I] = FVector4f(F.Flashes[I].Intensity, 0, 0, 0);
		}
		C.Modeling = RModel;
		C.SdfR16 = RR16;
		C.SdfBC1 = RBC1;
		C.SdfBC4 = RBC4;
		C.NoiseTex = RNoise;
		C.LightGrid = bGridReadable ? RGrid : GSystemTextures.GetVolumetricBlackDummy(GraphBuilder);
		C.LinearClamp = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		C.LinearWrap = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	};

	FRenderState::FTimerSet& TS = S.Timers[S.TimerFrame % 4];
	const bool bTime = S.QueryPool.IsValid() && !TS.bIssued;
	if (bTime) for (int32 K = 0; K < 5; ++K) TS.Q[K] = S.QueryPool->AllocateQuery();
	auto Stamp = [&](int32 K)
	{
		if (!bTime) return;
		FRHIRenderQuery* Q = TS.Q[K].GetQuery();
		GraphBuilder.AddPass(RDG_EVENT_NAME("CLTimer%d", K), ERDGPassFlags::NeverCull, [Q](FRHICommandListImmediate& RHICmdList) { RHICmdList.EndRenderQuery(Q); });
	};

	Stamp(0);
	// 1. light grid: all slices when the sun or the data changed, otherwise 4 of 32 slices (8 frames per sweep)
	int32 Slices = 0;
	{
		const bool bRebuild = !S.bGridValid || F.bLightGridDirty || S.GridRep != F.Settings.Rep || !S.GridSun.Equals(F.SunDir, 1e-3);
		int32 Start = 0, Count = LZ;
		if (!bRebuild) { Start = S.NextSlice; Count = 4; S.NextSlice = (S.NextSlice + 4) % LZ; }
		S.bGridValid = true; S.GridRep = F.Settings.Rep; S.GridSun = F.SunDir;
		FCLLightGridCS::FParameters* P = GraphBuilder.AllocParameters<FCLLightGridCS::FParameters>();
		FillCommon(P->Common, false);
		P->RWLightGrid = GraphBuilder.CreateUAV(RGrid);
		P->SliceStart = Start;
		P->SliceCount = Count;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CloudLightGrid %d slices", Count), TShaderMapRef<FCLLightGridCS>(Map), P, FIntVector(LX / 8, LY / 8, Count));
		Slices = Count;
	}
	Stamp(1);

	// 2. ray march, one or two layers
	const int32 Res = F.Settings.Resolution;
	const bool bSplit = Res == 2 && F.Settings.DebugView != 1 && F.Settings.DebugView != 2;	// heat maps count one full ray
	const int32 FarDiv = Res == 0 ? 1 : 2;
	const int32 NearDiv = 4;
	const FIntPoint FarSize(FMath::DivideAndRoundUp(ViewRect.Width(), FarDiv), FMath::DivideAndRoundUp(ViewRect.Height(), FarDiv));
	const FIntPoint NearSize(FMath::DivideAndRoundUp(ViewRect.Width(), NearDiv), FMath::DivideAndRoundUp(ViewRect.Height(), NearDiv));
	auto AddMarch = [&](const FIntPoint& Size, int32 Div, float T0, float T1, uint32 Tint, FRDGTextureRef& OutCol, FRDGTextureRef& OutDep, const TCHAR* Name)
	{
		OutCol = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_ShaderResource | TexCreate_UAV), Name);
		OutDep = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("CLCloudDepth"));
		FCLMarchCS::FParameters* P = GraphBuilder.AllocParameters<FCLMarchCS::FParameters>();
		FillCommon(P->Common, true);
		P->SceneDepth = SceneDepthTex;
		P->RWCloud = GraphBuilder.CreateUAV(OutCol);
		P->RWCloudDepth = GraphBuilder.CreateUAV(OutDep);
		P->LowSize = FUintVector2(Size.X, Size.Y);
		P->ResDiv = Div;
		P->ViewMin = ViewRect.Min;
		P->ViewMax = ViewRect.Max;
		P->TStartM = T0;
		P->TEndM = T1;
		P->LayerTint = Tint;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CloudMarch %s %dx%d", Name, Size.X, Size.Y), TShaderMapRef<FCLMarchCS>(Map), P, FComputeShaderUtils::GetGroupCount(Size, 8));
	};
	FRDGTextureRef NearCol = nullptr, NearDep = nullptr, FarCol = nullptr, FarDep = nullptr;
	if (bSplit) AddMarch(NearSize, NearDiv, 0.f, F.Settings.SplitM, 1, NearCol, NearDep, TEXT("CLNear"));
	Stamp(2);
	AddMarch(FarSize, FarDiv, bSplit ? F.Settings.SplitM : 0.f, 1e7f, 2, FarCol, FarDep, TEXT("CLFar"));
	Stamp(3);

	// 3. upsample and composite over scene colour (premultiplied alpha)
	{
		FCLCompositePS::FParameters* P = GraphBuilder.AllocParameters<FCLCompositePS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->SceneDepth = SceneDepthTex;
		P->FarCloud = FarCol; P->FarDepth = FarDep;
		P->NearCloud = bSplit ? NearCol : FarCol; P->NearDepth = bSplit ? NearDep : FarDep;
		P->FarSize = FUintVector2(FarSize.X, FarSize.Y);
		P->NearSize = bSplit ? FUintVector2(NearSize.X, NearSize.Y) : P->FarSize;
		P->FarDiv = FarDiv;
		P->NearDiv = bSplit ? NearDiv : FarDiv;
		P->UseNear = bSplit ? 1u : 0u;
		P->ViewMin = ViewRect.Min;
		P->RenderTargets[0] = FRenderTargetBinding(SceneColor, ERenderTargetLoadAction::ELoad);
		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, Map, RDG_EVENT_NAME("CloudComposite"), TShaderMapRef<FCLCompositePS>(Map), P, ViewRect,
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_One>::GetRHI());
	}
	Stamp(4);
	if (bTime)
	{
		TS.bIssued = true;
		TS.Near = bSplit ? NearSize : FIntPoint::ZeroValue;
		TS.Far = FarSize;
		TS.Slices = Slices;
	}
	++S.TimerFrame;
}

// ---------------------------------------------------------------- module

class FCloudLabShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		AddShaderSourceDirectoryMapping(TEXT("/CloudLab"), FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("Shaders")));
	}
};
IMPLEMENT_MODULE(FCloudLabShadersModule, CloudLabShaders);
