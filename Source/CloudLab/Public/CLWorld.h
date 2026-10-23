#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CLWorld.generated.h"

class UDirectionalLightComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UExponentialHeightFogComponent;
class UProceduralMeshComponent;
class UPostProcessComponent;

UENUM()
enum class ECLTimeOfDay : uint8 { Morning, Noon, Evening };

/** The archipelago under the clouds: terrain, sea, sun, sky and fog, all built in code at BeginPlay. */
UCLASS()
class CLOUDLAB_API ACLWorld : public AActor
{
	GENERATED_BODY()
public:
	ACLWorld();
	virtual void BeginPlay() override;
	static ACLWorld* Get(const UObject* Ctx);

	/** Terrain height (world cm) at a world XY position; the sea is at z = 0. */
	static float HeightCm(float X, float Y);
	/** Domain metres <-> world cm (the cloud grid starts at DomainBaseM above the sea, centred on the origin). */
	static FVector DomainToWorld(const FVector& M);
	static FVector WorldToDomain(const FVector& W);
	static FVector DomainMinCm();

	void SetTimeOfDay(ECLTimeOfDay T);
	ECLTimeOfDay GetTimeOfDay() const { return Tod; }
	FVector SunDir() const;			// towards the sun
	FLinearColor SunColor() const;	// for the cloud shader
	FLinearColor SkyColor() const;
	FLinearColor HazeColor() const;
	int32 SunVersion = 0;			// bumped when the sun moves

private:
	UPROPERTY() TObjectPtr<UDirectionalLightComponent> Sun;
	UPROPERTY() TObjectPtr<USkyAtmosphereComponent> Atmosphere;
	UPROPERTY() TObjectPtr<USkyLightComponent> SkyLight;
	UPROPERTY() TObjectPtr<UExponentialHeightFogComponent> Fog;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Terrain;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Sea;
	UPROPERTY() TObjectPtr<UPostProcessComponent> Post;
	ECLTimeOfDay Tod = ECLTimeOfDay::Morning;
	void BuildTerrain();
	void BuildSea();
};
