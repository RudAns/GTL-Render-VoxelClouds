#include "CLRace.h"

#include "CLCloudField.h"
#include "CLCloudManager.h"
#include "CLPlane.h"
#include "CLPlayerController.h"
#include "CLWorld.h"
#include "Components/PointLightComponent.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Sound/SoundBase.h"

// ================================================================ ring

ACLRing::ACLRing()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(Mesh);
	Glow->SetMobility(EComponentMobility::Movable);
	Glow->CastShadows = false;
	Glow->IntensityUnits = ELightUnits::Candelas;
}

void ACLRing::Setup(float R, const FString& InLabel)
{
	RadiusCm = R; Label = InLabel;
	// torus in the actor's YZ plane (the actor's X axis is the flight direction)
	const int32 NU = 56, NV = 10;
	const float Tube = 85.f;
	TArray<FVector> V; TArray<int32> T; TArray<FVector> N; TArray<FVector2D> UV; TArray<FLinearColor> C; TArray<FProcMeshTangent> Tan;
	for (int32 I = 0; I <= NU; ++I)
		for (int32 J = 0; J <= NV; ++J)
		{
			const float U = 2 * PI * I / NU, W = 2 * PI * J / NV;
			const FVector Ring(0, FMath::Cos(U), FMath::Sin(U));
			const FVector Nn = Ring * FMath::Cos(W) + FVector(FMath::Sin(W), 0, 0);
			V.Add(Ring * R + Nn * Tube); N.Add(Nn); UV.Add(FVector2D(I / float(NU), J / float(NV))); C.Add(FLinearColor::White);
		}
	for (int32 I = 0; I < NU; ++I)
		for (int32 J = 0; J < NV; ++J)
		{
			const int32 A = I * (NV + 1) + J, B = A + 1, D = A + NV + 1, E = D + 1;
			T.Append({ A, D, B, B, D, E });
		}
	Mesh->CreateMeshSection_LinearColor(0, V, T, N, UV, C, Tan, false);
	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Ring.M_Ring"), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		Mat = UMaterialInstanceDynamic::Create(M, this);
		Mesh->SetMaterial(0, Mat);
	}
	Glow->AttenuationRadius = R * 1.6f;
	SetState(0);
}

void ACLRing::SetState(int32 S)
{
	State = S;
	const FLinearColor Col = S == 1 ? FLinearColor(1.0f, 0.42f, 0.06f) : (S == 2 ? FLinearColor(0.18f, 0.75f, 0.35f) : FLinearColor(0.75f, 0.85f, 1.f));
	const float Em = S == 1 ? 4.f : (S == 2 ? 1.2f : 1.5f);
	if (Mat) { Mat->SetVectorParameterValue(TEXT("Color"), Col); Mat->SetScalarParameterValue(TEXT("Emissive"), Em); }
	Glow->SetIntensity(S == 1 ? 2500.f : 0.f);
	Glow->SetLightColor(Col);
	Glow->SetVisibility(S == 1);
}

// ================================================================ race

ACLRace::ACLRace()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

ACLRace* ACLRace::Get(const UObject* Ctx)
{
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W) return nullptr;
	for (TActorIterator<ACLRace> It(W); It; ++It) return *It;
	return nullptr;
}

void ACLRace::BeginPlay()
{
	Super::BeginPlay();
	Chime = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_Ring.S_Ring"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	CrashSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_Crash.S_Crash"), nullptr, LOAD_NoWarn | LOAD_Quiet);
}

ACLPlane* ACLRace::GetPlane() const
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	return PC ? Cast<ACLPlane>(PC->GetPawn()) : nullptr;
}

void ACLRace::SpawnRings()
{
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	if (!M || !M->GetField()) return;
	for (const FCLRingSpot& S : M->GetField()->Course)
	{
		FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACLRing* R = GetWorld()->SpawnActor<ACLRing>(ACLWorld::DomainToWorld(S.PosM), S.Dir.Rotation(), P);
		if (R) { R->Setup(S.RadiusM * 100.f, S.Label); Rings.Add(R); }
	}
	bSpawned = true;
}

float ACLRace::RaceSeconds() const
{
	const float Now = GetWorld()->GetTimeSeconds();
	if (State == ECLRaceState::Flying) return Now - StartTime + Penalty;
	if (State == ECLRaceState::Finished) return EndTime - StartTime + Penalty;
	if (State == ECLRaceState::Crashed) return EndTime - StartTime + Penalty;
	return 0.f;
}

void ACLRace::Event(const FString& S) { LastEvent = S; LastEventTime = GetWorld()->GetTimeSeconds(); }

void ACLRace::Restart()
{
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	ACLPlane* Pl = GetPlane();
	if (!M || !M->GetField() || !Pl) return;
	Next = 0; Penalty = 0.f; Crashes = 0;
	for (int32 I = 0; I < Rings.Num(); ++I) Rings[I]->SetState(I == 0 ? 1 : 0);
	Pl->ResetTo(ACLWorld::DomainToWorld(M->GetField()->StartM), M->GetField()->StartDir, 62.f);
	Pl->Throttle = 0.6f;
	Pl->bFrozen = true;
	State = ECLRaceState::Countdown;
	StateTime = GetWorld()->GetTimeSeconds();
	AutoT = 0.f;
}

void ACLRace::RespawnAtLastRing()
{
	ACLPlane* Pl = GetPlane();
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	if (!Pl || !M || !M->GetField()) return;
	FVector At = ACLWorld::DomainToWorld(M->GetField()->StartM), Dir = M->GetField()->StartDir;
	if (Next > 0 && Rings.IsValidIndex(Next - 1))
	{
		At = Rings[Next - 1]->GetActorLocation();
		Dir = Rings[Next - 1]->GetActorForwardVector();
	}
	Pl->ResetTo(At, Dir, 60.f);
	if (State == ECLRaceState::Crashed)
	{
		Penalty += 5.f;
		StartTime += GetWorld()->GetTimeSeconds() - EndTime;	// the clock was stopped while the wreck was shown
		State = ECLRaceState::Flying;
		Event(TEXT("回到上一个检查环 · 罚时 5 秒"));
	}
}

void ACLRace::Tick(float Dt)
{
	Super::Tick(Dt);
	ACLCloudManager* M = ACLCloudManager::Get(this);
	ACLPlane* Pl = GetPlane();
	if (!M || !Pl) return;
	const float Now = GetWorld()->GetTimeSeconds();
	if (bCaptureHold && State != ECLRaceState::Loading) return;
	if (State == ECLRaceState::Loading)
	{
		Pl->bFrozen = true;
		if (M->IsReady())
		{
			if (!bSpawned) SpawnRings();
			Restart();
		}
		return;
	}
	if (State == ECLRaceState::Countdown)
	{
		Pl->bFrozen = true;
		if (Now - StateTime > 3.f)
		{
			State = ECLRaceState::Flying; StartTime = Now; Pl->bFrozen = false;
			Event(TEXT("出发！"));
		}
		return;
	}
	if (State == ECLRaceState::Flying)
	{
		// self-test autopilot: aim at the next ring (or a little past it once close)
		if (bAutoPilot && Rings.IsValidIndex(Next))
		{
			// turn the aim toward the next ring at a limited rate, the way a pilot moves the mouse
			const FVector To = Rings[Next]->GetActorLocation() + Rings[Next]->GetActorForwardVector() * 1500.f - Pl->GetActorLocation();
			const FQuat From = Pl->Aim.Quaternion(), Want = To.Rotation().Quaternion();
			const float Ang = FMath::RadiansToDegrees(From.AngularDistance(Want));
			const float MaxStep = 120.f * Dt;
			Pl->Aim = (Ang <= MaxStep ? Want : FQuat::Slerp(From, Want, MaxStep / Ang)).Rotator();
			Pl->Aim.Roll = 0.f;
		}
		// ring passing: the segment flown this frame crosses the ring's plane inside its radius
		if (Rings.IsValidIndex(Next))
		{
			ACLRing* R = Rings[Next];
			const FVector C = R->GetActorLocation(), N = R->GetActorForwardVector();
			const FVector A = Pl->PrevLocation, B = Pl->GetActorLocation();
			const float Da = FVector::DotProduct(A - C, N), Db = FVector::DotProduct(B - C, N);
			if (Da < 0.f && Db >= 0.f)
			{
				const FVector X = A + (B - A) * (Da / (Da - Db));
				if (FVector::Dist(X, C) < R->RadiusCm + 150.f)
				{
					R->SetState(2);
					++Next;
					if (Chime) UGameplayStatics::PlaySound2D(this, Chime, 0.8f);
					if (Rings.IsValidIndex(Next)) { Rings[Next]->SetState(1); Event(FString::Printf(TEXT("通过「%s」"), *R->Label)); }
					else
					{
						State = ECLRaceState::Finished; EndTime = Now;
						const float T = RaceSeconds();
						if (Best <= 0.f || T < Best) Best = T;
						Event(TEXT("完成！"));
					}
				}
			}
		}
		if (Pl->bCrashed)
		{
			State = ECLRaceState::Crashed; EndTime = Now; ++Crashes;
			if (CrashSound) UGameplayStatics::PlaySound2D(this, CrashSound, 0.9f);
			Event(Pl->HeightAboveGroundM() < 3.f && ACLWorld::HeightCm(Pl->GetActorLocation().X, Pl->GetActorLocation().Y) < 0.f ? TEXT("坠海") : TEXT("撞上山体"));
		}
		// the play area ends a little beyond the cloud grid
		const FVector D = ACLWorld::WorldToDomain(Pl->GetActorLocation());
		if (D.X < -900 || D.Y < -900 || D.X > 4996 || D.Y > 4996)
		{
			Event(TEXT("飞出了空域，回到上一个检查环"));
			RespawnAtLastRing();
		}
	}
}

// ================================================================ game mode

ACLGameMode::ACLGameMode()
{
	DefaultPawnClass = ACLPlane::StaticClass();
	PlayerControllerClass = ACLPlayerController::StaticClass();
}

void ACLGameMode::StartPlay()
{
	UWorld* W = GetWorld();
	FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (!ACLWorld::Get(this)) W->SpawnActor<ACLWorld>(FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!ACLCloudManager::Get(this)) W->SpawnActor<ACLCloudManager>(FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!ACLRace::Get(this)) W->SpawnActor<ACLRace>(FVector::ZeroVector, FRotator::ZeroRotator, P);
	Super::StartPlay();
}
