#include "CLWorld.h"

#include "CLCloudField.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

namespace
{
	struct FIsland { float X, Y, R, H; };	// domain metres
	const FIsland Islands[] = {
		{ 430, 590, 360, 115 },		// start island
		{ 1290, 1420, 520, 300 },	// the mountain whose summit reaches the tunnel cloud's base
		{ 820, 1580, 260, 90 },
		{ 2280, 1880, 420, 150 },
		{ 1850, 2650, 380, 120 },
		{ 2950, 2830, 640, 230 },	// the big island under the stratus deck
		{ 3420, 3330, 470, 170 },	// storm island
		{ 3680, 1500, 420, 140 },
		{ 1200, 3300, 520, 180 },
		{ 600, 2400, 300, 100 },
		{ 3300, 600, 360, 120 },
		{ 2300, 600, 260, 80 },
	};
	float Hash2(int32 X, int32 Y) { uint32 H = uint32(X) * 374761393u + uint32(Y) * 668265263u; H = (H ^ (H >> 13)) * 1274126177u; return (H & 0xFFFF) / 65535.f; }
	float VNoise2(float X, float Y)
	{
		const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
		const float FX = X - X0, FY = Y - Y0, SX = FX * FX * (3 - 2 * FX), SY = FY * FY * (3 - 2 * FY);
		return FMath::Lerp(FMath::Lerp(Hash2(X0, Y0), Hash2(X0 + 1, Y0), SX), FMath::Lerp(Hash2(X0, Y0 + 1), Hash2(X0 + 1, Y0 + 1), SX), SY);
	}
	float Ridged(float X, float Y)
	{
		float A = 0.5f, F = 1.f, S = 0.f;
		for (int32 O = 0; O < 5; ++O) { S += A * (1.f - FMath::Abs(VNoise2(X * F, Y * F) * 2.f - 1.f)); A *= 0.5f; F *= 2.03f; }
		return S;
	}
}

ACLWorld::ACLWorld()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

ACLWorld* ACLWorld::Get(const UObject* Ctx)
{
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W) return nullptr;
	for (TActorIterator<ACLWorld> It(W); It; ++It) return *It;
	return nullptr;
}

FVector ACLWorld::DomainMinCm()
{
	using namespace CLRender;
	const float VoxelCm = FCLCloudField::VoxelM * 100.f;
	return FVector(-NX * VoxelCm * 0.5f, -NY * VoxelCm * 0.5f, FCLCloudField::DomainBaseM * 100.f);
}
FVector ACLWorld::DomainToWorld(const FVector& M) { return DomainMinCm() + M * 100.f; }
FVector ACLWorld::WorldToDomain(const FVector& W) { return (W - DomainMinCm()) * 0.01f; }

float ACLWorld::HeightCm(float X, float Y)
{
	const FVector D = WorldToDomain(FVector(X, Y, 0));
	float H = -40.f;
	for (const FIsland& I : Islands)
	{
		const float Dd = FMath::Sqrt(FMath::Square(D.X - I.X) + FMath::Square(D.Y - I.Y)) / I.R;
		if (Dd > 1.6f) continue;
		const float Bump = FMath::Exp(-Dd * Dd * 2.4f);
		const float Rd = Ridged(D.X / 420.f + I.X * 0.01f, D.Y / 420.f + I.Y * 0.01f);
		H = FMath::Max(H, -40.f + (I.H + 40.f) * Bump * (0.55f + 0.6f * Rd));
	}
	// gentle beaches: flatten the band just above the sea
	if (H > 0.f && H < 6.f) H = H * H / 6.f;
	return H * 100.f;
}

void ACLWorld::BeginPlay()
{
	Super::BeginPlay();
	// Every property is set before RegisterComponent: a sky light whose real-time capture is switched on after
	// registration never captures anything.
	Sun = NewObject<UDirectionalLightComponent>(this);
	Sun->SetupAttachment(RootComponent);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->Intensity = 8.f;
	Sun->bAtmosphereSunLight = true;
	Sun->CastShadows = true;
	Sun->LightSourceAngle = 0.6f;
	Sun->DynamicShadowDistanceMovableLight = 60000.f;
	Sun->RegisterComponent();
	Atmosphere = NewObject<USkyAtmosphereComponent>(this);
	Atmosphere->SetupAttachment(RootComponent);
	Atmosphere->RegisterComponent();
	SkyLight = NewObject<USkyLightComponent>(this);
	SkyLight->SetupAttachment(RootComponent);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;
	SkyLight->Intensity = 1.4f;
	SkyLight->bLowerHemisphereIsBlack = false;
	SkyLight->LowerHemisphereColor = FLinearColor(0.05f, 0.09f, 0.13f);
	SkyLight->RegisterComponent();
	Fog = NewObject<UExponentialHeightFogComponent>(this);
	Fog->SetupAttachment(RootComponent);
	Fog->FogDensity = 0.0035f;
	Fog->FogHeightFalloff = 0.08f;
	Fog->StartDistance = 1500.f;
	Fog->RegisterComponent();
	Post = NewObject<UPostProcessComponent>(this);
	Post->SetupAttachment(RootComponent);
	Post->bUnbound = true;
	Post->Settings.bOverride_AutoExposureMethod = true;
	Post->Settings.AutoExposureMethod = AEM_Manual;
	Post->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post->Settings.AutoExposureApplyPhysicalCameraExposure = false;
	Post->Settings.bOverride_AutoExposureBias = true;
	Post->Settings.AutoExposureBias = 0.3f;
	Post->RegisterComponent();
	BuildTerrain();
	BuildSea();
	SetTimeOfDay(ECLTimeOfDay::Morning);
}

void ACLWorld::BuildTerrain()
{
	// 6 km x 6 km heightfield, 16 m cells, centred on the cloud domain
	const int32 N = 384;
	const float Cell = 1600.f, Half = N * Cell * 0.5f;
	TArray<FVector> V; TArray<int32> Tri; TArray<FVector> Nrm; TArray<FVector2D> UV; TArray<FLinearColor> Col; TArray<FProcMeshTangent> Tan;
	V.SetNumUninitialized((N + 1) * (N + 1));
	for (int32 Y = 0; Y <= N; ++Y)
		for (int32 X = 0; X <= N; ++X)
		{
			const float WX = -Half + X * Cell, WY = -Half + Y * Cell;
			V[Y * (N + 1) + X] = FVector(WX, WY, HeightCm(WX, WY));
		}
	Nrm.SetNumUninitialized(V.Num()); UV.SetNumUninitialized(V.Num());
	for (int32 Y = 0; Y <= N; ++Y)
		for (int32 X = 0; X <= N; ++X)
		{
			const int32 I = Y * (N + 1) + X;
			const float HL = V[Y * (N + 1) + FMath::Max(X - 1, 0)].Z, HR = V[Y * (N + 1) + FMath::Min(X + 1, N)].Z;
			const float HD = V[FMath::Max(Y - 1, 0) * (N + 1) + X].Z, HU = V[FMath::Min(Y + 1, N) * (N + 1) + X].Z;
			Nrm[I] = FVector(HL - HR, HD - HU, 2.f * Cell).GetSafeNormal();
			UV[I] = FVector2D(V[I].X, V[I].Y) / 100000.f;
		}
	for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const int32 A = Y * (N + 1) + X, B = A + 1, C = A + N + 1, D = C + 1;
			// skip cells deep under the sea
			if (V[A].Z < -2500.f && V[B].Z < -2500.f && V[C].Z < -2500.f && V[D].Z < -2500.f) continue;
			Tri.Append({ A, C, B, B, C, D });
		}
	Terrain = NewObject<UProceduralMeshComponent>(this);
	Terrain->SetupAttachment(RootComponent);
	Terrain->bUseAsyncCooking = true;
	Terrain->CreateMeshSection_LinearColor(0, V, Tri, Nrm, UV, Col, Tan, false);
	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Terrain.M_Terrain"), nullptr, LOAD_NoWarn | LOAD_Quiet))
		Terrain->SetMaterial(0, M);
	Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Terrain->RegisterComponent();
}

void ACLWorld::BuildSea()
{
	// one large quad, subdivided a little so the depth precision and vertex fog behave
	const int32 N = 16;
	const float Size = 9000000.f, Half = Size * 0.5f;
	TArray<FVector> V; TArray<int32> Tri; TArray<FVector> Nrm; TArray<FVector2D> UV; TArray<FLinearColor> Col; TArray<FProcMeshTangent> Tan;
	for (int32 Y = 0; Y <= N; ++Y)
		for (int32 X = 0; X <= N; ++X)
		{
			V.Add(FVector(-Half + Size * X / N, -Half + Size * Y / N, 0.f));
			Nrm.Add(FVector::UpVector);
			UV.Add(FVector2D(X, Y));
		}
	for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const int32 A = Y * (N + 1) + X, B = A + 1, C = A + N + 1, D = C + 1;
			Tri.Append({ A, C, B, B, C, D });
		}
	Sea = NewObject<UProceduralMeshComponent>(this);
	Sea->SetupAttachment(RootComponent);
	Sea->CreateMeshSection_LinearColor(0, V, Tri, Nrm, UV, Col, Tan, false);
	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Sea.M_Sea"), nullptr, LOAD_NoWarn | LOAD_Quiet))
		Sea->SetMaterial(0, M);
	Sea->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sea->SetCastShadow(false);
	Sea->RegisterComponent();
}

void ACLWorld::SetTimeOfDay(ECLTimeOfDay T)
{
	Tod = T;
	// pitch = -elevation; yaw = direction the light travels
	FRotator R;
	switch (T)
	{
	case ECLTimeOfDay::Morning: R = FRotator(-14.f, 205.f, 0.f); break;
	case ECLTimeOfDay::Noon: R = FRotator(-58.f, 150.f, 0.f); break;
	default: R = FRotator(-9.f, 40.f, 0.f); break;
	}
	if (Sun) Sun->SetWorldRotation(R);
	++SunVersion;
}

FVector ACLWorld::SunDir() const
{
	return Sun ? -Sun->GetForwardVector() : FVector(0.3, 0.2, 0.9).GetSafeNormal();
}

FLinearColor ACLWorld::SunColor() const
{
	// sunlight reaching the clouds (roughly what the atmosphere leaves of 8 lux at this elevation)
	const float Elev = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SunDir().Z, -1.0, 1.0)));
	const float W = FMath::Clamp((Elev - 4.f) / 40.f, 0.f, 1.f);
	return FMath::Lerp(FLinearColor(9.0f, 5.6f, 3.2f), FLinearColor(9.2f, 8.8f, 8.2f), W) * FMath::Lerp(0.85f, 1.f, W);
}

FLinearColor ACLWorld::SkyColor() const
{
	const float Elev = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SunDir().Z, -1.0, 1.0)));
	const float W = FMath::Clamp((Elev - 4.f) / 40.f, 0.f, 1.f);
	return FMath::Lerp(FLinearColor(1.2f, 1.25f, 1.55f), FLinearColor(1.5f, 2.0f, 2.8f), W);
}

FLinearColor ACLWorld::HazeColor() const
{
	const float Elev = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SunDir().Z, -1.0, 1.0)));
	const float W = FMath::Clamp((Elev - 4.f) / 40.f, 0.f, 1.f);
	return FMath::Lerp(FLinearColor(1.15f, 0.95f, 0.85f), FLinearColor(1.0f, 1.25f, 1.6f), W);
}
