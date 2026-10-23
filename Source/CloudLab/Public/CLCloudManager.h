#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CLRender.h"
#include "CLCloudManager.generated.h"

class FCLCloudField;
class UVolumetricCloudComponent;
class UVolumeTexture;
class UMaterialInstanceDynamic;

/** Which renderer draws the clouds: ours, or UE's own Volumetric Cloud component fed with the same voxel data. */
UENUM()
enum class ECLRoute : uint8 { Custom, UECloud };

/** Owns the cloudscape, the renderer and the storm's lightning; everything else asks it about the clouds. */
UCLASS()
class CLOUDLAB_API ACLCloudManager : public AActor
{
	GENERATED_BODY()
public:
	ACLCloudManager();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float Dt) override;
	static ACLCloudManager* Get(const UObject* Ctx);

	bool IsReady() const { return bReady; }
	const FCLCloudField* GetField() const { return Field.Get(); }

	/** Cloud density (profile x density scale) at a world position, in the representation being drawn. */
	float DensityAt(const FVector& World) const;
	float SdfAt(const FVector& World) const;		// metres to the nearest cloud
	bool InStorm(const FVector& World) const;

	FCLRenderSettings Render;
	ECLRoute Route = ECLRoute::Custom;
	void SetRoute(ECLRoute R);
	FCLGpuTimes Gpu;
	float BuildSeconds = 0.f;

	// lightning
	struct FStrike { FVector Pos; float Born; bool bGround; };
	TArray<FStrike> Strikes;
	float LastStrikeTime = -100.f;
	FVector LastStrikePos = FVector::ZeroVector;
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStrike, const FVector&, bool);
	FOnStrike OnStrike;
	void ForceStrike(const FVector& World);

private:
	TSharedPtr<FCLCloudRenderer, ESPMode::ThreadSafe> Renderer;
	TSharedPtr<FCLCloudField> Field;
	TFuture<void> Building;
	bool bReady = false;
	bool bBuildStarted = false;
	double BuildStart = 0;
	int32 LastSunVersion = -1;
	uint32 FrameIndex = 0;
	FVector WindM = FVector::ZeroVector;
	float NextStrike = 4.f;
	void Uploaded();

	// UE comparison route
	UPROPERTY() TObjectPtr<UVolumetricCloudComponent> UECloud;
	UPROPERTY() TObjectPtr<UVolumeTexture> UEModel;
	UPROPERTY() TObjectPtr<UVolumeTexture> UENoise;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> UEMat;
	void MakeUECloud();
};
