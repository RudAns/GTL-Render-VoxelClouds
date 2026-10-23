#pragma once

#include "CoreMinimal.h"
#include "CLRender.h"

/**
 * The cloudscape: a procedural stand-in for the talk's Houdini "frankencloudscape" (Guerrilla's own clouds are
 * not used). Cumulus towers are unions of flattened ellipsoids with a flat base and a displaced surface,
 * tunnels and caves are carved out, then the whole area is written into one dense 512 x 512 x 64 grid of 8 m
 * voxels: dimensional profile (gradient from the shape's surface inward), detail type, density scale and a
 * zone tag. The SDF is an exact Euclidean distance transform of the occupancy, like the talk's separate SDF NVDF.
 *
 * Positions here are domain metres: x, y in [0, 4096], z in [0, 512]; world z = DomainBaseM + z.
 */
struct FCLRingSpot
{
	FVector PosM = FVector::ZeroVector;	// domain metres
	FVector Dir = FVector::ForwardVector;	// flight direction through the ring
	float RadiusM = 14.f;
	FString Label;
};

class CLOUDLAB_API FCLCloudField
{
public:
	static constexpr float VoxelM = 8.f;
	static constexpr float DomainBaseM = 200.f;	// world altitude of the grid's floor

	/** Build everything (a few seconds, multithreaded). */
	void Build(int32 Seed);

	// render data per representation (voxel / 2.5D envelope)
	FCLVolumeData Data[2];
	TArray<TArray<uint8>> NoiseMips;

	// gameplay queries (domain metres)
	float Density(const FVector& PM, ECLRepresentation Rep) const;	// profile x density scale, trilinear
	float Sdf(const FVector& PM, ECLRepresentation Rep) const;		// metres, trilinear of the exact SDF
	bool InStorm(const FVector& PM) const;

	TArray<FCLRingSpot> Course;
	FVector StartM = FVector::ZeroVector, StartDir = FVector::ForwardVector;
	FVector StormCoreM = FVector::ZeroVector;
	float StormRadiusM = 0.f;

	// build statistics for the HUD and the self-test
	struct FStats
	{
		float ShapeMs = 0, EdtMs = 0, EnvelopeMs = 0, EncodeMs = 0, NoiseMs = 0;
		float Occupancy = 0, EnvelopeOccupancy = 0;
		int64 BC1SafeOver = 0, BC1NaiveOver = 0, BC4Over = 0;	// texels whose decoded SDF can exceed the true one (decoder upper bound)
		float BC1SafeMeanUnder = 0, BC4MeanUnder = 0;
		float FilledTunnel = 0;	// envelope density on the voxel tunnel's centre line (> 0: the envelope fills it)
	} Stats;

private:
	TArray<float> SdfM[2];		// exact signed distance (m) per voxel
	TArray<uint8> Zone;
	void Shape(int32 Seed, TArray<float>& S, TArray<uint8>& Type, TArray<uint8>& DScale);
	void Envelope(const TArray<uint8>& Model, TArray<uint8>& Env);
	static void SignedEdt(const TArray<uint8>& Occ, TArray<float>& Out);
	static void Encode(const TArray<float>& Sdf, FCLVolumeData& D, FStats& St, bool bStats);
	void MakeNoise();
};
