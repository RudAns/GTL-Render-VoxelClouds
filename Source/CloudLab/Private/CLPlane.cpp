#include "CLPlane.h"

#include "Camera/CameraComponent.h"
#include "CLCloudField.h"
#include "CLCloudManager.h"
#include "CLWorld.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Sound/SoundBase.h"

// ================================================================ procedural model

namespace
{
	struct FMB
	{
		TArray<FVector> V; TArray<int32> T; TArray<FLinearColor> C; TArray<FVector2D> UV;
		int32 Add(const FVector& P, const FLinearColor& Col) { UV.Add(FVector2D(P.X, P.Y) * 0.01f); C.Add(Col); return V.Add(P); }
		void Tri(int32 A, int32 B, int32 D) { T.Append({ A, B, D }); }
		void Quad(int32 A, int32 B, int32 Cc, int32 D) { Tri(A, B, Cc); Tri(A, Cc, D); }
		// rings of equal size, closed; caps both ends with a fan
		void Loft(const TArray<TArray<FVector>>& Rings, TFunctionRef<FLinearColor(const FVector&)> Col, bool bCapStart = true, bool bCapEnd = true)
		{
			const int32 K = Rings[0].Num();
			TArray<int32> Base;
			for (const TArray<FVector>& R : Rings) { Base.Add(V.Num()); for (const FVector& P : R) Add(P, Col(P)); }
			for (int32 I = 0; I + 1 < Rings.Num(); ++I)
				for (int32 J = 0; J < K; ++J)
				{
					const int32 A = Base[I] + J, B = Base[I] + (J + 1) % K, D = Base[I + 1] + J, E = Base[I + 1] + (J + 1) % K;
					Quad(A, D, E, B);
				}
			auto Cap = [&](int32 Ri, bool bFlip)
			{
				FVector Ctr = FVector::ZeroVector; for (const FVector& P : Rings[Ri]) Ctr += P; Ctr /= K;
				const int32 M = Add(Ctr, Col(Ctr));
				const int32 Start = V.Num();
				for (const FVector& P : Rings[Ri]) Add(P, Col(P));
				for (int32 J = 0; J < K; ++J) { const int32 A = Start + J, B = Start + (J + 1) % K; if (bFlip) Tri(M, B, A); else Tri(M, A, B); }
			};
			if (bCapStart) Cap(0, false);
			if (bCapEnd) Cap(Rings.Num() - 1, true);
		}
		void Commit(UProceduralMeshComponent* PM, int32 Section)
		{
			// smooth vertex normals from the faces; duplicated vertices (caps) keep their own
			TArray<FVector> N; N.Init(FVector::ZeroVector, V.Num());
			for (int32 I = 0; I + 2 < T.Num(); I += 3)
			{
				const FVector Fn = FVector::CrossProduct(V[T[I + 2]] - V[T[I]], V[T[I + 1]] - V[T[I]]);
				N[T[I]] += Fn; N[T[I + 1]] += Fn; N[T[I + 2]] += Fn;
			}
			for (FVector& X : N) X = X.GetSafeNormal();
			TArray<FProcMeshTangent> Tan;
			PM->CreateMeshSection_LinearColor(Section, V, T, N, UV, C, Tan, false);
		}
	};

	const FLinearColor White(0.86f, 0.87f, 0.85f), Red(0.62f, 0.05f, 0.04f), Belly(0.55f, 0.57f, 0.58f), Dark(0.03f, 0.035f, 0.04f), Yellow(0.9f, 0.62f, 0.05f), Metal(0.35f, 0.36f, 0.37f);

	TArray<FVector> Airfoil(float X0, float X1, float Chord, const FVector& Le, float Thick, bool bVertical)
	{
		// symmetric NACA 00xx thickness, chord along -X from the leading edge, optional vertical orientation (fins)
		auto Yt = [Thick](float X) { return 5.f * Thick * (0.2969f * FMath::Sqrt(FMath::Max(X, 0.f)) - 0.126f * X - 0.3516f * X * X + 0.2843f * X * X * X - 0.1036f * X * X * X * X); };
		const int32 S = 9;
		TArray<FVector> R;
		for (int32 I = 0; I <= S; ++I) { const float X = FMath::Lerp(X0, X1, 0.5f - 0.5f * FMath::Cos(PI * I / S)); const float H = Yt(X) * Chord; R.Add(bVertical ? Le + FVector(-X * Chord, H, 0) : Le + FVector(-X * Chord, 0, H)); }
		for (int32 I = S - 1; I >= 1; --I) { const float X = FMath::Lerp(X0, X1, 0.5f - 0.5f * FMath::Cos(PI * I / S)); const float H = Yt(X) * Chord; R.Add(bVertical ? Le + FVector(-X * Chord, -H, 0) : Le + FVector(-X * Chord, 0, -H)); }
		return R;
	}

	// a wing panel between two spanwise stations; chord fraction X0..X1; positions relative to Origin
	void WingPanel(FMB& M, float Y0, float Y1, float Le0, float Le1, float C0, float C1, float Z0, float Z1, float X0, float X1, const FVector& Origin, const FLinearColor& Col, bool bCaps)
	{
		TArray<TArray<FVector>> Rings;
		for (int32 I = 0; I <= 4; ++I)
		{
			const float A = I / 4.f;
			const float Y = FMath::Lerp(Y0, Y1, A), Le = FMath::Lerp(Le0, Le1, A), C = FMath::Lerp(C0, C1, A), Z = FMath::Lerp(Z0, Z1, A);
			TArray<FVector> R = Airfoil(X0, X1, C, FVector(Le, Y, Z), 0.13f, false);
			for (FVector& P : R) P -= Origin;
			Rings.Add(R);
		}
		M.Loft(Rings, [&](const FVector&) { return Col; }, bCaps, bCaps);
	}

	void Ellipsoid(FMB& M, const FVector& C, const FVector& R, const FLinearColor& Col, bool bUpperOnly = false, int32 Seg = 14)
	{
		TArray<TArray<FVector>> Rings;
		const int32 Lat = 8;
		for (int32 I = 1; I < Lat; ++I)
		{
			const float Th = (bUpperOnly ? 0.5f * PI : PI) * I / Lat - (bUpperOnly ? 0.f : 0.5f * PI);
			TArray<FVector> Ring;
			for (int32 J = 0; J < Seg; ++J)
			{
				const float Ph = 2 * PI * J / Seg;
				Ring.Add(C + FVector(FMath::Cos(Th) * FMath::Cos(Ph) * R.X, FMath::Cos(Th) * FMath::Sin(Ph) * R.Y, FMath::Sin(Th) * R.Z));
			}
			Rings.Add(Ring);
		}
		M.Loft(Rings, [&](const FVector&) { return Col; }, true, true);
	}
}

ACLPlane::ACLPlane()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	auto PM = [this](const TCHAR* Name) { UProceduralMeshComponent* C = CreateDefaultSubobject<UProceduralMeshComponent>(Name); C->SetupAttachment(RootComponent); C->SetCollisionEnabled(ECollisionEnabled::NoCollision); return C; };
	Body = PM(TEXT("Body"));
	Canopy = PM(TEXT("Canopy"));
	Prop = PM(TEXT("Prop"));
	AileronL = PM(TEXT("AileronL"));
	AileronR = PM(TEXT("AileronR"));
	Elevator = PM(TEXT("Elevator"));
	Rudder = PM(TEXT("Rudder"));
	Trails = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Trails"));
	Trails->SetupAttachment(RootComponent);
	Trails->SetUsingAbsoluteLocation(true); Trails->SetUsingAbsoluteRotation(true); Trails->SetUsingAbsoluteScale(true);
	Trails->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Trails->SetCastShadow(false);
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->SetUsingAbsoluteLocation(true); Camera->SetUsingAbsoluteRotation(true);
	Camera->FieldOfView = 75.f;
	Engine = CreateDefaultSubobject<UAudioComponent>(TEXT("Engine"));
	Engine->SetupAttachment(RootComponent);
	Engine->bAutoActivate = false;
	Wind = CreateDefaultSubobject<UAudioComponent>(TEXT("Wind"));
	Wind->SetupAttachment(RootComponent);
	Wind->bAutoActivate = false;
	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void ACLPlane::BuildModel()
{
	UMaterialInterface* Paint = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Paint.M_Paint"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Glass = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Canopy.M_Canopy"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Trail = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/CloudLab/Materials/M_Trail.M_Trail"), nullptr, LOAD_NoWarn | LOAD_Quiet);

	// fuselage: superelliptic sections along X (cm), nose at +X
	{
		FMB M;
		struct FSt { float X, W, H, Z; };
		const FSt St[] = { { 330, 74, 74, 2 }, { 300, 88, 90, 3 }, { 240, 98, 106, 6 }, { 150, 100, 114, 10 }, { 60, 96, 110, 10 },
			{ -40, 82, 94, 14 }, { -140, 62, 74, 20 }, { -240, 42, 54, 28 }, { -320, 26, 38, 34 }, { -362, 12, 24, 38 } };
		TArray<TArray<FVector>> Rings;
		const int32 K = 16;
		for (const FSt& S : St)
		{
			TArray<FVector> R;
			for (int32 J = 0; J < K; ++J)
			{
				const float A = 2 * PI * J / K;
				const float Ca = FMath::Cos(A), Sa = FMath::Sin(A), E = 2.f / 2.6f;
				R.Add(FVector(S.X, S.W * 0.5f * FMath::Sign(Ca) * FMath::Pow(FMath::Abs(Ca), E), S.Z + S.H * 0.5f * FMath::Sign(Sa) * FMath::Pow(FMath::Abs(Sa), E)));
			}
			Rings.Add(R);
		}
		M.Loft(Rings, [](const FVector& P)
		{
			if (P.X > 250.f) return Red;						// cowl
			const float Zc = 10.f + (P.X < 0 ? -P.X * 0.08f : 0.f);
			if (FMath::Abs(P.Z - Zc) < 7.f && P.X < 230.f && P.X > -330.f) return Red;	// side stripe
			return P.Z < Zc - 25.f ? Belly : White;
		});
		// wing: low wing with 4 deg dihedral, root chord 175 cm, tip 115 cm, span 840 cm; inner panel and the fixed part of the outer panel
		const float Root = 50.f, Mid = 230.f, Tip = 420.f;
		auto Le = [](float Y) { return FMath::Lerp(130.f, 95.f, (Y - 50.f) / 370.f); };
		auto Ch = [](float Y) { return FMath::Lerp(175.f, 115.f, (Y - 50.f) / 370.f); };
		auto Zw = [](float Y) { return -34.f + (Y - 50.f) * FMath::Tan(FMath::DegreesToRadians(4.f)); };
		for (float Sg : { 1.f, -1.f })
		{
			WingPanel(M, Sg * 0.f, Sg * Root, 140, Le(Root), 190, Ch(Root), -36, Zw(Root), 0, 1, FVector::ZeroVector, White, true);
			WingPanel(M, Sg * Root, Sg * Mid, Le(Root), Le(Mid), Ch(Root), Ch(Mid), Zw(Root), Zw(Mid), 0, 1, FVector::ZeroVector, White, true);
			WingPanel(M, Sg * Mid, Sg * Tip, Le(Mid), Le(Tip), Ch(Mid), Ch(Tip), Zw(Mid), Zw(Tip), 0, 0.74f, FVector::ZeroVector, White, true);
			Ellipsoid(M, FVector(Le(Tip) - Ch(Tip) * 0.45f, Sg * (Tip + 4.f), Zw(Tip)), FVector(Ch(Tip) * 0.5f, 6.f, 7.f), Red);	// tip cap
			// landing gear: strut and spat
			Ellipsoid(M, FVector(115, Sg * 118, -118), FVector(42, 13, 20), Red);
			FMB& Mm = M;
			TArray<TArray<FVector>> Strut;
			for (float Z : { -40.f, -108.f }) { TArray<FVector> R; for (int32 J = 0; J < 6; ++J) { const float A = 2 * PI * J / 6; R.Add(FVector(118 + 6 * FMath::Cos(A), Sg * (60 + (Z + 40) * -0.85f) + 3 * FMath::Sin(A), Z)); } Strut.Add(R); }
			Mm.Loft(Strut, [](const FVector&) { return Metal; });
			// horizontal stabiliser (fixed front part)
			WingPanel(M, Sg * 10.f, Sg * 150.f, -270, -300, 85, 58, 34, 36, 0, 0.55f, FVector::ZeroVector, White, true);
		}
		// vertical fin (fixed front part)
		{
			TArray<TArray<FVector>> Rings2;
			for (int32 I = 0; I <= 4; ++I)
			{
				const float A = I / 4.f, Z = FMath::Lerp(40.f, 160.f, A), LeX = FMath::Lerp(-250.f, -318.f, A), C = FMath::Lerp(105.f, 55.f, A);
				Rings2.Add(Airfoil(0, 0.6f, C, FVector(LeX, 0, Z), 0.12f, true));
			}
			M.Loft(Rings2, [](const FVector& P) { return P.Z > 130.f ? Red : White; });
		}
		M.Commit(Body, 0);
		if (Paint) Body->SetMaterial(0, Paint);
	}
	// control surfaces: built around their hinge line so the component rotation deflects them
	auto Surface = [&](UProceduralMeshComponent* C, TFunctionRef<void(FMB&)> Build, const FVector& Hinge)
	{
		FMB M; Build(M); M.Commit(C, 0);
		if (Paint) C->SetMaterial(0, Paint);
		C->SetRelativeLocation(Hinge);
	};
	{
		auto Le = [](float Y) { return FMath::Lerp(130.f, 95.f, (Y - 50.f) / 370.f); };
		auto Ch = [](float Y) { return FMath::Lerp(175.f, 115.f, (Y - 50.f) / 370.f); };
		auto Zw = [](float Y) { return -34.f + (Y - 50.f) * FMath::Tan(FMath::DegreesToRadians(4.f)); };
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Sg = Side == 0 ? -1.f : 1.f;
			const FVector Hinge(Le(325) - Ch(325) * 0.74f, Sg * 325.f, Zw(325));
			Surface(Side == 0 ? AileronL : AileronR, [&](FMB& M) { WingPanel(M, Sg * 230.f, Sg * 420.f, Le(230), Le(420), Ch(230), Ch(420), Zw(230), Zw(420), 0.74f, 1.f, Hinge, Red, true); }, Hinge);
		}
		const FVector EH(-270 - 70 * 0.55f, 0, 35);
		Surface(Elevator, [&](FMB& M) { for (float Sg : { 1.f, -1.f }) WingPanel(M, Sg * 10.f, Sg * 150.f, -270, -300, 85, 58, 34, 36, 0.55f, 1.f, EH, White, true); }, EH);
		const FVector RH(-250 - 105 * 0.6f, 0, 40);
		Surface(Rudder, [&](FMB& M)
		{
			TArray<TArray<FVector>> Rings;
			for (int32 I = 0; I <= 4; ++I)
			{
				const float A = I / 4.f, Z = FMath::Lerp(40.f, 160.f, A), LeX = FMath::Lerp(-250.f, -318.f, A), C = FMath::Lerp(105.f, 55.f, A);
				TArray<FVector> R = Airfoil(0.6f, 1.f, C, FVector(LeX, 0, Z), 0.12f, true);
				for (FVector& P : R) P -= RH;
				Rings.Add(R);
			}
			M.Loft(Rings, [](const FVector&) { return Red; });
		}, RH);
	}
	// canopy
	{
		FMB M; Ellipsoid(M, FVector(95, 0, 56), FVector(78, 34, 36), Dark, true, 16); M.Commit(Canopy, 0);
		if (Glass) Canopy->SetMaterial(0, Glass); else if (Paint) Canopy->SetMaterial(0, Paint);
	}
	// spinner and three blades
	{
		FMB M;
		TArray<TArray<FVector>> Sp;
		for (int32 I = 0; I <= 5; ++I)
		{
			const float A = I / 5.f, R = 30.f * FMath::Sqrt(1.f - A * A * 0.98f);
			TArray<FVector> Ring; for (int32 J = 0; J < 12; ++J) { const float B = 2 * PI * J / 12; Ring.Add(FVector(A * 48.f, R * FMath::Cos(B), R * FMath::Sin(B))); }
			Sp.Add(Ring);
		}
		M.Loft(Sp, [](const FVector&) { return Red; });
		for (int32 B = 0; B < 3; ++B)
		{
			const float A = 2 * PI * B / 3;
			const FVector Ax(0, FMath::Cos(A), FMath::Sin(A)), Side(0, -FMath::Sin(A), FMath::Cos(A));
			TArray<TArray<FVector>> Bl;
			for (int32 I = 0; I <= 5; ++I)
			{
				const float R = FMath::Lerp(22.f, 98.f, I / 5.f), Tw = FMath::DegreesToRadians(FMath::Lerp(38.f, 14.f, I / 5.f)), C = FMath::Lerp(16.f, 9.f, I / 5.f);
				const FVector Ctr = Ax * R + FVector(10, 0, 0);
				const FVector Chord = (Side * FMath::Cos(Tw) + FVector(-1, 0, 0) * FMath::Sin(Tw)) * C * 0.5f;
				const FVector Thick = (FVector(1, 0, 0) * FMath::Cos(Tw) + Side * FMath::Sin(Tw)) * 1.6f;
				Bl.Add({ Ctr - Chord, Ctr - Thick, Ctr + Chord, Ctr + Thick });
			}
			M.Loft(Bl, [](const FVector& P) { return FVector(0, P.Y, P.Z).Size() > 86.f ? Yellow : Dark; });
		}
		M.Commit(Prop, 0);
		if (Paint) Prop->SetMaterial(0, Paint);
		Prop->SetRelativeLocation(FVector(334, 0, 2));
	}
	if (Trail) Trails->SetMaterial(0, Trail);
}

// ================================================================ behaviour

void ACLPlane::BeginPlay()
{
	Super::BeginPlay();
	BuildModel();
	USoundBase* EngineS = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_Engine.S_Engine"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	USoundBase* WindS = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_Wind.S_Wind"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	ThunderNear = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_ThunderNear.S_ThunderNear"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	ThunderFar = LoadObject<USoundBase>(nullptr, TEXT("/Game/CloudLab/Audio/S_ThunderFar.S_ThunderFar"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (EngineS) { Engine->SetSound(EngineS); Engine->Play(); }
	if (WindS) { Wind->SetSound(WindS); Wind->SetVolumeMultiplier(0.f); Wind->Play(); }
	Orient = GetActorQuat();
	Aim = GetActorRotation();
	PrevLocation = GetActorLocation();
	if (ACLCloudManager* M = ACLCloudManager::Get(this)) M->OnStrike.AddUObject(this, &ACLPlane::OnLightning);
}

void ACLPlane::ResetTo(const FVector& Where, const FVector& Dir, float Speed)
{
	SetActorLocation(Where);
	Orient = Dir.ToOrientationQuat();
	SetActorRotation(Orient);
	Aim = Dir.Rotation();
	SpeedMs = Speed;
	Ice = 0.f; bStalled = false; ShockT = 0.f; bCrashed = false;
	AngVel = FVector::ZeroVector;
	PrevLocation = Where;
	bCamInit = false;
	TrailL.Reset(); TrailR.Reset();
}

float ACLPlane::AltitudeM() const { return GetActorLocation().Z * 0.01f; }
float ACLPlane::HeightAboveGroundM() const
{
	const FVector P = GetActorLocation();
	return (P.Z - FMath::Max(ACLWorld::HeightCm(P.X, P.Y), 0.f)) * 0.01f;
}

void ACLPlane::OnLightning(const FVector& Where, bool bGround)
{
	const float D = FVector::Dist(Where, GetActorLocation()) * 0.01f;
	if (D < 260.f && bInStorm) ShockT = 1.4f;
	// thunder arrives at the speed of sound
	Thunder.Add({ float(GetWorld()->GetTimeSeconds() + D / 343.f), D < 500.f, FMath::Clamp(1.5f - D / 1500.f, 0.25f, 1.f) });
}

void ACLPlane::Tick(float Dt)
{
	Super::Tick(Dt);
	Dt = FMath::Min(Dt, 0.05f);
	PrevLocation = GetActorLocation();
	if (!bFrozen && !bCrashed) Fly(Dt);
	// control surfaces and propeller
	PropAngle = FMath::Fmod(PropAngle + Dt * (900.f + 1500.f * Throttle), 360.f);
	Prop->SetRelativeRotation(FRotator(0, 0, PropAngle));
	const float Ail = FMath::Clamp(AngVel.X / 150.f, -1.f, 1.f) * 22.f, Ele = FMath::Clamp(AngVel.Y / 60.f, -1.f, 1.f) * 20.f, Rud = FMath::Clamp(AngVel.Z / 30.f, -1.f, 1.f) * 20.f;
	AileronL->SetRelativeRotation(FRotator(Ail, 0, 0));
	AileronR->SetRelativeRotation(FRotator(-Ail, 0, 0));
	Elevator->SetRelativeRotation(FRotator(-Ele, 0, 0));
	Rudder->SetRelativeRotation(FRotator(0, -Rud, 0));
	UpdateTrails(Dt);
	UpdateCamera(Dt);
	UpdateAudio(Dt);
}

void ACLPlane::Fly(float Dt)
{
	ACLCloudManager* M = ACLCloudManager::Get(this);
	const FVector P = GetActorLocation();
	CloudDensity = M ? M->DensityAt(P) : 0.f;
	bInStorm = M && M->InStorm(P) && CloudDensity > 0.01f;

	// icing: flying inside dense cloud loads the airframe with ice; clear air melts it
	if (CloudDensity > 0.04f) Ice = FMath::Min(1.f, Ice + Dt * 0.55f * CloudDensity);
	else Ice = FMath::Max(0.f, Ice - Dt * 0.16f);
	if (Ice >= 1.f) bStalled = true;
	if (bStalled && Ice < 0.55f) bStalled = false;
	ShockT = FMath::Max(0.f, ShockT - Dt);

	Throttle = FMath::Clamp(Throttle + ThrottleInput * Dt * 0.6f, 0.f, 1.f);

	// steering toward the aim direction: bank into the turn, pull the nose up in the body frame
	const FVector AimDir = Aim.Vector();
	const FVector L = Orient.UnrotateVector(AimDir);
	const float ErrPitch = FMath::RadiansToDegrees(FMath::Atan2(L.Z, FMath::Max(L.X, 0.05f)));
	const float ErrYaw = FMath::RadiansToDegrees(FMath::Atan2(L.Y, FMath::Max(L.X, 0.05f)));
	// world-frame heading error decides the bank angle
	const FVector Fwd = Orient.GetForwardVector();
	const FVector FlatF = FVector(Fwd.X, Fwd.Y, 0).GetSafeNormal(), FlatA = FVector(AimDir.X, AimDir.Y, 0).GetSafeNormal();
	const float Heading = FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(FlatF, FlatA).Z, FVector::DotProduct(FlatF, FlatA)));
	const float Roll = FMath::RadiansToDegrees(FMath::Atan2(Orient.GetRightVector().Z, Orient.GetUpVector().Z));	// positive = right wing down... computed below
	const float WantBank = FMath::Clamp(Heading * 2.0f, -78.f, 78.f) + RollInput * 70.f;
	const float CurBank = -Roll;
	const float Ctrl = (bStalled ? 0.35f : 1.f) * FMath::Lerp(1.f, 0.6f, Ice);
	FVector Want;
	Want.X = FMath::Clamp((WantBank - CurBank) * 3.5f, -170.f, 170.f) * Ctrl;	// roll rate
	Want.Y = FMath::Clamp(ErrPitch * 3.0f, -75.f, 75.f) * Ctrl;					// pitch rate (nose up positive)
	Want.Z = FMath::Clamp(ErrYaw * 1.6f, -28.f, 28.f) * Ctrl;					// yaw rate
	if (bStalled) Want.Y = -28.f;	// iced up: the nose drops
	if (ShockT > 0.f) { const float Tm = GetWorld()->GetTimeSeconds(); Want += FVector(FMath::Sin(Tm * 37.f) * 90.f, FMath::Sin(Tm * 23.f) * 25.f, 0.f) * (ShockT / 1.4f); }
	// turbulence inside clouds
	Turb = FMath::Lerp(Turb, CloudDensity, 1.f - FMath::Exp(-3.f * Dt));
	if (Turb > 0.02f) { const float Tm = GetWorld()->GetTimeSeconds(); Want += FVector(FMath::Sin(Tm * 5.1f) * 14.f, FMath::Sin(Tm * 6.7f + 1.f) * 6.f, FMath::Sin(Tm * 4.3f) * 3.f) * FMath::Min(Turb * 2.f, 1.f); }
	AngVel = FMath::Lerp(AngVel, Want, 1.f - FMath::Exp(-7.f * Dt));
	const FQuat DRoll(FVector::ForwardVector, FMath::DegreesToRadians(AngVel.X * Dt));
	const FQuat DPitch(FVector::RightVector, FMath::DegreesToRadians(-AngVel.Y * Dt));
	const FQuat DYaw(FVector::UpVector, FMath::DegreesToRadians(AngVel.Z * Dt));
	Orient = (Orient * DYaw * DPitch * DRoll).GetNormalized();
	GForce = 1.f + FMath::Abs(AngVel.Y) * FMath::DegreesToRadians(1.f) * SpeedMs / 9.81f;

	// speed: throttle sets a target, climbing costs speed, ice costs thrust
	const float Target = FMath::Lerp(40.f, 92.f, Throttle) * (1.f - 0.42f * Ice);
	const FVector F2 = Orient.GetForwardVector();
	SpeedMs += ((Target - SpeedMs) * 0.55f - 9.81f * F2.Z * 0.55f) * Dt;
	SpeedMs = FMath::Clamp(SpeedMs, 28.f, 110.f);
	FVector Vel = F2 * SpeedMs;
	if (bStalled) Vel.Z -= 9.f;
	FVector NewP = P + Vel * Dt * 100.f;
	// the cloud grid is the play area: a soft ceiling just above it
	NewP.Z = FMath::Min(NewP.Z, (FCLCloudField::DomainBaseM + 560.f) * 100.f);
	SetActorLocationAndRotation(NewP, Orient);
	// crash into the ground or the sea
	const float Ground = FMath::Max(ACLWorld::HeightCm(NewP.X, NewP.Y), 0.f);
	if (NewP.Z < Ground + 120.f) bCrashed = true;
}

void ACLPlane::UpdateCamera(float Dt)
{
	const FVector P = GetActorLocation();
	FVector WantPos; FQuat WantRot;
	if (bCockpitCam)
	{
		WantPos = P + Orient.RotateVector(FVector(70, 0, 95));
		WantRot = Orient;
		CamPos = WantPos; CamRot = WantRot;
	}
	else
	{
		// chase camera behind the aim direction (mouse-aim style), lifted a little; smoothed but never lagging behind the plane
		const FVector AimDir = Aim.Vector();
		WantPos = P - AimDir * 1450.f + FVector::UpVector * 320.f;
		WantRot = FRotationMatrix::MakeFromXZ(P + AimDir * 4000.f - WantPos, FVector::UpVector).ToQuat();
		if (!bCamInit) { CamPos = WantPos; CamRot = WantRot; bCamInit = true; }
		const float A = 1.f - FMath::Exp(-14.f * Dt);
		// follow the plane rigidly and smooth only the offset, so the plane does not jitter on screen
		const FVector Off = FMath::Lerp(CamPos - PrevLocation, WantPos - P, A);
		CamPos = P + Off;
		CamRot = FQuat::Slerp(CamRot, WantRot, 1.f - FMath::Exp(-18.f * Dt));
	}
	FVector Shake = FVector::ZeroVector;
	const float Tm = GetWorld()->GetTimeSeconds();
	const float Amp = Turb * 30.f + ShockT * 25.f;
	if (Amp > 0.5f) Shake = FVector(0, FMath::Sin(Tm * 31.f), FMath::Sin(Tm * 27.f + 2.f)) * Amp;
	Camera->SetWorldLocationAndRotation(CamPos + CamRot.RotateVector(Shake), CamRot);
	Camera->FieldOfView = FMath::Lerp(72.f, 84.f, FMath::Clamp((SpeedMs - 40.f) / 60.f, 0.f, 1.f));
}

void ACLPlane::UpdateTrails(float Dt)
{
	// wingtip vapour: emitted while inside cloud (as in the talk's flight footage) or pulling hard
	const float Now = GetWorld()->GetTimeSeconds();
	const float Emit = FMath::Clamp(FMath::Max(CloudDensity * 6.f, (GForce - 2.2f) * 0.5f), 0.f, 1.f);
	auto Push = [&](TArray<FTrailPt>& Tr, const FVector& Local)
	{
		const FVector W = GetActorLocation() + Orient.RotateVector(Local);
		if (Emit > 0.02f || (Tr.Num() && Tr.Last().Alpha > 0.f)) Tr.Add({ W, Now, Emit });
		Tr.RemoveAll([Now](const FTrailPt& T) { return Now - T.Born > 1.6f; });
		if (Tr.Num() > 120) Tr.RemoveAt(0, Tr.Num() - 120);
	};
	Push(TrailL, FVector(60, -424, -10));
	Push(TrailR, FVector(60, 424, -10));
	TArray<FVector> V; TArray<int32> T; TArray<FVector> N; TArray<FVector2D> UV; TArray<FLinearColor> C; TArray<FProcMeshTangent> Tan;
	const FVector CamP = Camera->GetComponentLocation();
	for (const TArray<FTrailPt>* Tr : { &TrailL, &TrailR })
	{
		if (Tr->Num() < 2) continue;
		const int32 Base = V.Num();
		for (int32 I = 0; I < Tr->Num(); ++I)
		{
			const FTrailPt& Pt = (*Tr)[I];
			const FVector Dir = ((*Tr)[FMath::Min(I + 1, Tr->Num() - 1)].P - (*Tr)[FMath::Max(I - 1, 0)].P).GetSafeNormal();
			const FVector Side = FVector::CrossProduct(Dir, (CamP - Pt.P).GetSafeNormal()).GetSafeNormal();
			const float Age = Now - Pt.Born, W = 14.f + Age * 70.f;
			const float A = Pt.Alpha * FMath::Clamp(1.f - Age / 1.6f, 0.f, 1.f);
			V.Add(Pt.P - Side * W); V.Add(Pt.P + Side * W);
			N.Add(FVector::UpVector); N.Add(FVector::UpVector);
			UV.Add(FVector2D(0, Age)); UV.Add(FVector2D(1, Age));
			C.Add(FLinearColor(1, 1, 1, A)); C.Add(FLinearColor(1, 1, 1, A));
			if (I > 0) { const int32 J = Base + I * 2; T.Append({ J - 2, J - 1, J, J - 1, J + 1, J }); }
		}
	}
	if (V.Num()) Trails->CreateMeshSection_LinearColor(0, V, T, N, UV, C, Tan, false);
	else Trails->ClearAllMeshSections();
}

void ACLPlane::UpdateAudio(float Dt)
{
	if (Engine && Engine->IsPlaying())
	{
		Engine->SetPitchMultiplier(FMath::Lerp(0.75f, 1.35f, Throttle) * (ShockT > 0.f ? 0.85f : 1.f));
		Engine->SetVolumeMultiplier(bCrashed ? 0.f : FMath::Lerp(0.45f, 0.8f, Throttle) * (bCockpitCam ? 1.f : 0.75f));
	}
	if (Wind && Wind->IsPlaying())
		Wind->SetVolumeMultiplier(bCrashed ? 0.f : FMath::Clamp((SpeedMs - 30.f) / 70.f, 0.f, 1.f) * 0.55f + Turb * 0.9f);
	const float Now = GetWorld()->GetTimeSeconds();
	for (int32 I = Thunder.Num() - 1; I >= 0; --I)
		if (Now >= Thunder[I].At)
		{
			USoundBase* S = Thunder[I].bNear ? ThunderNear : ThunderFar;
			if (S) UGameplayStatics::PlaySound2D(this, S, Thunder[I].Vol);
			Thunder.RemoveAt(I);
		}
}
