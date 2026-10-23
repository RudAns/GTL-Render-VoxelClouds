#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "CLRace.generated.h"

class UProceduralMeshComponent;
class UMaterialInstanceDynamic;
class USoundBase;
class UPointLightComponent;
class ACLPlane;

UCLASS()
class CLOUDLAB_API ACLRing : public AActor
{
	GENERATED_BODY()
public:
	ACLRing();
	void Setup(float RadiusCm, const FString& InLabel);
	void SetState(int32 S);		// 0 ahead, 1 next, 2 passed
	FString Label;
	float RadiusCm = 1500.f;
	int32 State = 0;
private:
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Mat;
	UPROPERTY() TObjectPtr<UPointLightComponent> Glow;
};

UENUM()
enum class ECLRaceState : uint8 { Loading, Countdown, Flying, Finished, Crashed };

/** The time trial: rings in order through the cloudscape, a countdown, a clock, crash and restart. */
UCLASS()
class CLOUDLAB_API ACLRace : public AActor
{
	GENERATED_BODY()
public:
	ACLRace();
	virtual void BeginPlay() override;
	virtual void Tick(float Dt) override;
	static ACLRace* Get(const UObject* Ctx);

	void Restart();
	void RespawnAtLastRing();
	ACLPlane* GetPlane() const;

	ECLRaceState State = ECLRaceState::Loading;
	TArray<TObjectPtr<ACLRing>> Rings;
	int32 Next = 0;
	float StartTime = 0.f, EndTime = 0.f, StateTime = 0.f;
	float Penalty = 0.f;
	float Best = 0.f;
	int32 Crashes = 0;
	float RaceSeconds() const;
	FString LastEvent;
	float LastEventTime = -10.f;
	bool bAutoPilot = false;	// self-test: steer at the next ring
	bool bCaptureHold = false;	// self-test: freeze the race while the camera is placed by script
	float AutoT = 0.f;

private:
	UPROPERTY() TObjectPtr<USoundBase> Chime;
	UPROPERTY() TObjectPtr<USoundBase> CrashSound;
	bool bSpawned = false;
	void SpawnRings();
	void Event(const FString& S);
};

UCLASS()
class CLOUDLAB_API ACLGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	ACLGameMode();
	virtual void StartPlay() override;
};
