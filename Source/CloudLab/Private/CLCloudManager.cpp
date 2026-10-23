#include "CLCloudManager.h"

#include "Async/Async.h"
#include "CLCloudField.h"
#include "CLWorld.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/VolumeTexture.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SceneViewExtension.h"

ACLCloudManager::ACLCloudManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

ACLCloudManager* ACLCloudManager::Get(const UObject* Ctx)
{
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W) return nullptr;
	for (TActorIterator<ACLCloudManager> It(W); It; ++It) return *It;
	return nullptr;
}

void ACLCloudManager::BeginPlay()
{
	Super::BeginPlay();
	Renderer = FSceneViewExtensions::NewExtension<FCLCloudRenderer>(GetWorld());
	Field = MakeShared<FCLCloudField>();
	bBuildStarted = true;
	BuildStart = FPlatformTime::Seconds();
	TSharedPtr<FCLCloudField> F = Field;
	Building = Async(EAsyncExecution::Thread, [F]() { F->Build(20231103); });
}

void ACLCloudManager::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Building.IsValid()) Building.Wait();
	Renderer.Reset();
	Super::EndPlay(Reason);
}

void ACLCloudManager::Uploaded()
{
	Renderer->UploadVolumes(ECLRepresentation::Voxel, Field->Data[0]);
	Renderer->UploadVolumes(ECLRepresentation::Envelope, Field->Data[1]);
	Renderer->UploadNoise(Field->NoiseMips);
	BuildSeconds = FPlatformTime::Seconds() - BuildStart;
	bReady = true;
	UE_LOG(LogTemp, Display, TEXT("CLTEST cloud build %.1f s (shape %.0f ms, EDT %.0f ms, envelope %.0f ms, encode %.0f ms, noise %.0f ms), occupancy %.3f / envelope %.3f"),
		BuildSeconds, Field->Stats.ShapeMs, Field->Stats.EdtMs, Field->Stats.EnvelopeMs, Field->Stats.EncodeMs, Field->Stats.NoiseMs, Field->Stats.Occupancy, Field->Stats.EnvelopeOccupancy);
}

float ACLCloudManager::DensityAt(const FVector& World) const
{
	if (!bReady) return 0.f;
	return Field->Density(ACLWorld::WorldToDomain(World), Render.Rep);
}

float ACLCloudManager::SdfAt(const FVector& World) const
{
	if (!bReady) return 1e4f;
	return Field->Sdf(ACLWorld::WorldToDomain(World), Render.Rep);
}

bool ACLCloudManager::InStorm(const FVector& World) const
{
	return bReady && Field->InStorm(ACLWorld::WorldToDomain(World));
}

void ACLCloudManager::ForceStrike(const FVector& World)
{
	const float Now = GetWorld()->GetTimeSeconds();
	Strikes.Add({ World, Now, false });
	LastStrikeTime = Now; LastStrikePos = World;
	OnStrike.Broadcast(World, false);
}

void ACLCloudManager::Tick(float Dt)
{
	Super::Tick(Dt);
	if (bBuildStarted && !bReady && Building.IsValid() && Building.IsReady()) Uploaded();
	if (!bReady || !Renderer.IsValid()) return;
	ACLWorld* W = ACLWorld::Get(this);
	const float Now = GetWorld()->GetTimeSeconds();

	// lightning inside the storm cloud: a flash every few seconds, mostly in the lair, sometimes a ground strike
	if (Render.bLightning && Field->StormRadiusM > 0.f && Now > NextStrike)
	{
		FRandomStream R(int32(Now * 1000.f));
		const bool bGround = R.FRand() < 0.3f;
		FVector P = Field->StormCoreM + FVector(R.FRandRange(-140, 140), R.FRandRange(-140, 140), R.FRandRange(-60, 120));
		if (bGround) P = Field->StormCoreM + FVector(R.FRandRange(-60, 60), R.FRandRange(-60, 60), -150);
		const FVector Pw = ACLWorld::DomainToWorld(P);
		Strikes.Add({ Pw, Now, bGround });
		LastStrikeTime = Now; LastStrikePos = Pw;
		OnStrike.Broadcast(Pw, bGround);
		NextStrike = Now + R.FRandRange(2.2f, 5.5f);
	}
	Strikes.RemoveAll([Now](const FStrike& S) { return Now - S.Born > 0.9f; });

	WindM += FVector(3.0f, 1.2f, 0.f) * Dt;
	FCLRenderFrame F;
	F.Settings = Render;
	F.Settings.bEnabled = Route == ECLRoute::Custom;
	F.DomainMin = ACLWorld::DomainMinCm();
	F.VoxelCm = FCLCloudField::VoxelM * 100.f;
	if (W)
	{
		F.SunDir = W->SunDir();
		F.SunColor = W->SunColor();
		F.SkyColor = W->SkyColor();
		F.HazeColor = W->HazeColor();
		if (W->SunVersion != LastSunVersion) { F.bLightGridDirty = true; LastSunVersion = W->SunVersion; }
	}
	F.Time = Now;
	F.FrameIndex = FrameIndex++;
	F.WindM = WindM;
	for (const FStrike& S : Strikes)
	{
		if (F.Flashes.Num() >= CLRender::MaxFlashes) break;
		const float Age = Now - S.Born;
		// a strike flickers: a bright return stroke, a dip and one or two re-strikes
		const float I = FMath::Exp(-Age * 6.f) * (0.65f + 0.35f * FMath::Sin(Age * 60.f) * FMath::Sin(Age * 23.f));
		F.Flashes.Add({ S.Pos, S.bGround ? 380.f : 300.f, FMath::Max(I, 0.f) });
	}
	Renderer->SetFrame(F);
	Gpu = Renderer->GetGpuTimes();
	if (UECloud) UECloud->SetVisibility(Route == ECLRoute::UECloud);
}

void ACLCloudManager::SetRoute(ECLRoute R)
{
	Route = R;
	if (R == ECLRoute::UECloud && !UECloud && bReady) MakeUECloud();
}

void ACLCloudManager::MakeUECloud()
{
	// UE's Volumetric Cloud component drawing the same voxel data: the modelling NVDF and the noise become
	// transient volume textures read by a Volume-domain cloud material (built by Tools/build_assets.py).
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_UECloud.M_UECloud"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UE_LOG(LogTemp, Display, TEXT("CLTEST UE cloud route: material %s"), Base ? *Base->GetPathName() : TEXT("missing"));
	if (!Base || !Field.IsValid()) return;
	using namespace CLRender;
	auto MakeTex = [](int32 X, int32 Y, int32 Z, const TArray<uint8>& Rgba) -> UVolumeTexture*
	{
		UVolumeTexture* T = UVolumeTexture::CreateTransient(X, Y, Z, PF_B8G8R8A8);
		if (!T || !T->GetPlatformData() || T->GetPlatformData()->Mips.Num() == 0) return nullptr;
		FTexture2DMipMap& Mip = T->GetPlatformData()->Mips[0];
		uint8* Dst = (uint8*)Mip.BulkData.Lock(LOCK_READ_WRITE);
		for (int32 I = 0; I < X * Y * Z; ++I)
		{
			Dst[I * 4 + 0] = Rgba[I * 4 + 2]; Dst[I * 4 + 1] = Rgba[I * 4 + 1]; Dst[I * 4 + 2] = Rgba[I * 4 + 0]; Dst[I * 4 + 3] = Rgba[I * 4 + 3];
		}
		Mip.BulkData.Unlock();
		T->SRGB = false;
		T->Filter = TF_Trilinear;
		T->AddressMode = TA_Clamp;
		T->UpdateResource();
		return T;
	};
	UEModel = MakeTex(NX, NY, NZ, Field->Data[0].Modeling);
	UENoise = MakeTex(NoiseN, NoiseN, NoiseN, Field->NoiseMips[0]);
	if (UENoise) { UENoise->AddressMode = TA_Wrap; UENoise->UpdateResource(); }
	UEMat = UMaterialInstanceDynamic::Create(Base, this);
	UEMat->SetTextureParameterValue(TEXT("Model"), UEModel);
	UEMat->SetTextureParameterValue(TEXT("Noise"), UENoise);
	const FVector Min = ACLWorld::DomainMinCm();
	UEMat->SetVectorParameterValue(TEXT("DomainMin"), FLinearColor(Min.X, Min.Y, Min.Z, 0));
	UEMat->SetVectorParameterValue(TEXT("DomainSize"), FLinearColor(NX * 800.f, NY * 800.f, NZ * 800.f, 0));
	UECloud = NewObject<UVolumetricCloudComponent>(this);
	UECloud->SetupAttachment(RootComponent);
	UECloud->SetLayerBottomAltitude(FCLCloudField::DomainBaseM * 0.001f);
	UECloud->SetLayerHeight(NZ * FCLCloudField::VoxelM * 0.001f);
	UECloud->SetTracingMaxDistance(6.f);
	UECloud->SetViewSampleCountScale(8.f);
	UECloud->SetShadowViewSampleCountScale(4.f);
	UECloud->SetMaterial(UEMat);
	UECloud->RegisterComponent();
	UE_LOG(LogTemp, Display, TEXT("CLTEST UE cloud component registered: model %s noise %s"), UEModel ? TEXT("ok") : TEXT("none"), UENoise ? TEXT("ok") : TEXT("none"));
}
