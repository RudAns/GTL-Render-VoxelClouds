#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "CLPlane.generated.h"

class UCameraComponent;
class UProceduralMeshComponent;
class UAudioComponent;
class USoundBase;

/** A small aerobatic monoplane built in code. Arcade "mouse aim" flight: the plane steers toward where the camera looks. */
UCLASS()
class CLOUDLAB_API ACLPlane : public APawn
{
	GENERATED_BODY()
public:
	ACLPlane();
	virtual void BeginPlay() override;
	virtual void Tick(float Dt) override;

	// pilot input (set by the controller every frame)
	FRotator Aim = FRotator::ZeroRotator;	// where the pilot wants to fly
	float ThrottleInput = 0.f;				// -1..1 while W/S held
	float RollInput = 0.f;					// -1..1 A/D
	bool bCockpitCam = false;

	// state
	FQuat Orient = FQuat::Identity;
	float SpeedMs = 62.f;
	float Throttle = 0.6f;
	float Ice = 0.f;			// 0..1
	bool bStalled = false;
	float ShockT = 0.f;			// seconds of lightning shock left
	float CloudDensity = 0.f;	// density at the plane now
	bool bInStorm = false;
	bool bCrashed = false;
	bool bFrozen = false;		// countdown: hold position
	FVector PrevLocation = FVector::ZeroVector;
	float GForce = 1.f;
	FVector AngVel = FVector::ZeroVector;	// body rates roll, pitch, yaw (deg/s)

	void ResetTo(const FVector& Where, const FVector& Dir, float Speed);
	FVector NoseDir() const { return Orient.GetForwardVector(); }
	float AltitudeM() const;		// above the sea
	float HeightAboveGroundM() const;
	void OnLightning(const FVector& Where, bool bGround);
	UCameraComponent* GetCamera() const { return Camera; }

private:
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Body;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Canopy;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Prop;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> AileronL;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> AileronR;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Elevator;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Rudder;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Trails;
	UPROPERTY() TObjectPtr<UCameraComponent> Camera;
	UPROPERTY() TObjectPtr<UAudioComponent> Engine;
	UPROPERTY() TObjectPtr<UAudioComponent> Wind;
	UPROPERTY() TObjectPtr<USoundBase> ThunderNear;
	UPROPERTY() TObjectPtr<USoundBase> ThunderFar;
	FVector CamPos = FVector::ZeroVector;
	FQuat CamRot = FQuat::Identity;
	bool bCamInit = false;
	float PropAngle = 0.f;
	float Turb = 0.f;
	struct FTrailPt { FVector P; float Born; float Alpha; };
	TArray<FTrailPt> TrailL, TrailR;
	struct FPendingThunder { float At; bool bNear; float Vol; };
	TArray<FPendingThunder> Thunder;
	void BuildModel();
	void Fly(float Dt);
	void UpdateCamera(float Dt);
	void UpdateTrails(float Dt);
	void UpdateAudio(float Dt);
};
