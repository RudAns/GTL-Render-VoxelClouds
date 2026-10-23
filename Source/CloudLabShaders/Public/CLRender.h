#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

// Nubis^3-style voxel cloud renderer, written as a scene view extension (the report's main route C).
// One dense grid covers the whole play area (512 x 512 x 64 voxels of 8 m): a modelling field
// (dimensional profile, detail type, density scale) and a separate signed distance field. The shader
// up-rezzes the 8 m profile with a 128^3 noise texture, skips empty space with the SDF and reads most of
// the sun light from a precomputed 256 x 256 x 32 light grid.

namespace CLRender
{
	static constexpr int32 NX = 512, NY = 512, NZ = 64;
	static constexpr int32 LX = 256, LY = 256, LZ = 32;
	static constexpr int32 NoiseN = 128;
	static constexpr int32 MaxFlashes = 4;
	static constexpr float SdfLo = -256.f, SdfHi = 4096.f;	// metres, the range used by the talk
}

/** Which data set is drawn: the voxel cloudscape or its 2.5D envelope (per-column min/max height). */
enum class ECLRepresentation : uint8 { Voxel, Envelope };
enum class ECLSdfFormat : uint8 { R16, BC1Safe, BC1Naive, BC4Log, Count };

struct FCLRenderSettings
{
	bool bEnabled = true;
	ECLRepresentation Rep = ECLRepresentation::Voxel;
	int32 StepMode = 1;			// 0 2.5D distance-based step (3 + 60 d / 16384 m), 1 adaptive max(1, 0.08 sqrt d)
	bool bSdfSkip = true;		// step = max(sdf, step) outside clouds, sample only where sdf < 0
	ECLSdfFormat SdfFormat = ECLSdfFormat::BC1Safe;
	int32 MaxSteps = 1024;		// loop budget per ray
	int32 Detail = 2;			// 0 dimensional profile only, 1 + noise up-rez, 2 + twice-folded near detail
	bool bNoiseMipByDistance = true;
	int32 Lighting = 1;			// 0 ten-sample light march, 1 two samples + light grid
	bool bMultiScatter = true, bAmbient = true, bAmbientDirectional = true, bLightning = true;
	int32 Resolution = 2;		// 0 full, 1 half, 2 split: < SplitM at quarter, beyond at half
	float SplitM = 200.f;
	int32 DebugView = 0;		// 0 clouds, 1 loop iterations, 2 in-cloud samples, 3 light grid, 4 resolution split, 5 SDF overshoot, 6 SDF slice
	float DensityMul = 1.f;
};

struct FCLFlash
{
	FVector Pos = FVector::ZeroVector;	// world cm
	float RadiusM = 600.f;
	float Intensity = 0.f;				// 0..1 now
};

struct FCLRenderFrame
{
	FCLRenderSettings Settings;
	FVector DomainMin = FVector::ZeroVector;	// world cm, corner of voxel (0,0,0)
	float VoxelCm = 800.f;
	FVector SunDir = FVector(0.3, 0.2, 0.8);	// towards the sun
	FLinearColor SunColor = FLinearColor(9.f, 8.5f, 7.8f);
	FLinearColor SkyColor = FLinearColor(1.6f, 2.0f, 2.6f);
	FLinearColor HazeColor = FLinearColor(0.55f, 0.68f, 0.85f);
	float HazeKm = 18.f;
	float Time = 0.f;
	uint32 FrameIndex = 0;
	FVector WindM = FVector::ZeroVector;		// noise scroll offset (m), the talk's pseudo-motion
	TArray<FCLFlash> Flashes;
	bool bLightGridDirty = false;				// sun moved or data changed: rebuild every slice now
};

/** GPU times of the last completed frame, in milliseconds. */
struct FCLGpuTimes
{
	float LightMs = 0.f, NearMs = 0.f, FarMs = 0.f, CompositeMs = 0.f;
	FIntPoint NearSize = FIntPoint::ZeroValue, FarSize = FIntPoint::ZeroValue;
	int32 LightSlices = 0;
};

/** CPU-built volumes handed to the renderer once (or when the cloudscape changes). */
struct FCLVolumeData
{
	TArray<uint8> Modeling;		// NX*NY*NZ*4: profile, detail type, density scale, zone (255 = storm cloud)
	TArray<uint16> SdfR16;		// NX*NY*NZ, round-down UNORM of (sdf - lo) / (hi - lo)
	TArray<uint8> SdfBC1Safe, SdfBC1Naive, SdfBC4Log;	// block data, (NX/4)*(NY/4)*NZ blocks of 8 bytes
};

class CLOUDLABSHADERS_API FCLCloudRenderer : public FWorldSceneViewExtension
{
public:
	FCLCloudRenderer(const FAutoRegister& AutoReg, UWorld* InWorld);
	virtual ~FCLCloudRenderer() override;

	// --- game thread ---
	void SetFrame(const FCLRenderFrame& Frame);
	void UploadVolumes(ECLRepresentation Rep, const FCLVolumeData& Data);
	void UploadNoise(const TArray<TArray<uint8>>& Mips);	// RGBA8, NoiseN^3 then halves
	FCLGpuTimes GetGpuTimes() const;
	bool HasData() const { return bHasData; }

	// --- ISceneViewExtension ---
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const struct FPostProcessingInputs& Inputs) override;

private:
	struct FRenderState;
	TUniquePtr<FRenderState> RT;
	mutable FCriticalSection TimesLock;
	FCLGpuTimes Times;
	TAtomic<bool> bHasData { false };
	void SetTimes(const FCLGpuTimes& T) { FScopeLock L(&TimesLock); Times = T; }
};
