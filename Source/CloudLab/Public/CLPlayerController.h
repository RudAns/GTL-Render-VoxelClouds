#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "CLRender.h"
#include "CLPlayerController.generated.h"

/** Every switch of the principle panel. */
UENUM()
enum class ECLLab : uint8
{
	// route (whole replacement)
	RouteCustom, RouteUE,
	// representation
	RepVoxel, RepEnvelope,
	// detail (layered)
	Uprez, NearFold, NoiseMip,
	// ray march
	StepLegacy, StepAdaptive, SdfSkip, SdfR16, SdfBC1, SdfBC1Naive, SdfBC4, Cap128, Cap256, Cap512, CapNone,
	// lighting
	LightMarch, LightGrid, MultiScatter, Ambient, AmbientDir, Lightning,
	// resolution
	ResFull, ResHalf, ResSplit,
	// debug views
	ViewNormal, ViewIters, ViewSamples, ViewLight, ViewSplit, ViewOver,
	// time of day
	TodMorning, TodNoon, TodEvening,
	// actions
	Restart, Respawn, CockpitCam,
	Count
};

UCLASS()
class CLOUDLAB_API ACLPlayerController : public APlayerController
{
	GENERATED_BODY()
public:
	ACLPlayerController();
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float Dt) override;

	void Lab(ECLLab Which);
	bool LabOn(ECLLab Which) const;
	bool LabEnabled(ECLLab Which) const;
	static FText LabName(ECLLab Which);
	static FText LabHint(ECLLab Which);
	static FString LabKey(ECLLab Which);

	class ACLPlane* GetPlane() const;
	bool IsPanelOpen() const { return bPanel; }
	bool IsHelpOpen() const { return bHelp; }
	void TogglePanel();

	// --- self-test (-CLCapture=<dir>) ---
	FString CaptureDir;
	float CaptureT = 0.f;
	TMap<FString, float> Metrics;
	TArray<FCLGpuTimes> GpuHist;	// last frames' GPU timers, averaged by Perf()
	void Shot(const FString& Name);
	void Check(bool bOk, const FString& Name, const FString& Detail);
	void Perf(const FString& Name);
	void Hover(const FVector& DomainM, const FVector& Dir);
	void TickCapture(float Dt);
	float FrameMs = 16.f;

private:
	UPROPERTY() TObjectPtr<class UCLHud> Hud;
	bool bPanel = false;
	bool bHelp = true;
	float AimYaw = 0.f, AimPitch = 0.f;
	bool bAimInit = false;
	void OnTurn(float V);
	void OnLookUp(float V);
	void OnHelp() { bHelp = !bHelp; }
	void OnCycleView();
	void OnCam() { Lab(ECLLab::CockpitCam); }
	void OnRespawn() { Lab(ECLLab::Respawn); }
	void OnRestart() { Lab(ECLLab::Restart); }
};
