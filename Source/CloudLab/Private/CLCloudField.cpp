#include "CLCloudField.h"

#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"

using namespace CLRender;

namespace
{
	FORCEINLINE int32 Idx(int32 X, int32 Y, int32 Z) { return (Z * NY + Y) * NX + X; }

	// ------------------------------------------------------------ small noise helpers
	FORCEINLINE uint32 Hash3(int32 X, int32 Y, int32 Z, uint32 Seed)
	{
		uint32 H = (uint32)X * 73856093u ^ (uint32)Y * 19349663u ^ (uint32)Z * 83492791u ^ Seed * 2654435761u;
		H ^= H >> 13; H *= 0x5bd1e995u; H ^= H >> 15;
		return H;
	}
	FORCEINLINE float Rand01(uint32 H) { return (H & 0xFFFFFF) / float(0x1000000); }

	float ValueNoise(const FVector& P, uint32 Seed)
	{
		const int32 X0 = FMath::FloorToInt(P.X), Y0 = FMath::FloorToInt(P.Y), Z0 = FMath::FloorToInt(P.Z);
		const float FX = P.X - X0, FY = P.Y - Y0, FZ = P.Z - Z0;
		const float SX = FX * FX * (3 - 2 * FX), SY = FY * FY * (3 - 2 * FY), SZ = FZ * FZ * (3 - 2 * FZ);
		float V[8];
		for (int32 I = 0; I < 8; ++I) V[I] = Rand01(Hash3(X0 + (I & 1), Y0 + ((I >> 1) & 1), Z0 + (I >> 2), Seed));
		const float A = FMath::Lerp(V[0], V[1], SX), B = FMath::Lerp(V[2], V[3], SX), C = FMath::Lerp(V[4], V[5], SX), D = FMath::Lerp(V[6], V[7], SX);
		return FMath::Lerp(FMath::Lerp(A, B, SY), FMath::Lerp(C, D, SY), SZ);
	}
	float Fbm(const FVector& P, uint32 Seed)
	{
		return ValueNoise(P, Seed) * 0.55f + ValueNoise(P * 2.07f, Seed + 1) * 0.3f + ValueNoise(P * 4.13f, Seed + 2) * 0.15f;
	}

	FORCEINLINE float SMin(float A, float B, float K) { const float H = FMath::Max(K - FMath::Abs(A - B), 0.f) / K; return FMath::Min(A, B) - H * H * K * 0.25f; }
	FORCEINLINE float SMax(float A, float B, float K) { return -SMin(-A, -B, K); }
	FORCEINLINE float SdEllipsoid(const FVector& P, const FVector& R)
	{
		const float K0 = (P / R).Size();
		const float K1 = (P / (R * R)).Size();
		return K1 > 1e-6f ? K0 * (K0 - 1.f) / K1 : -FMath::Min3(R.X, R.Y, R.Z);
	}
	float DistToPolyline(const FVector& P, const TArray<FVector>& L)
	{
		float Best = 1e9f;
		for (int32 I = 0; I + 1 < L.Num(); ++I)
		{
			const FVector A = L[I], AB = L[I + 1] - A;
			const float T = FMath::Clamp(FVector::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1e-3f), 0.f, 1.f);
			Best = FMath::Min(Best, FVector::Dist(P, A + AB * T));
		}
		return Best;
	}

	// ------------------------------------------------------------ cloud descriptions
	enum class EKind : uint8 { Cumulus, Storm, Stratus };
	struct FPuff { FVector P; FVector R; };
	struct FTube { TArray<FVector> Path; float R; };
	struct FCloud
	{
		EKind Kind = EKind::Cumulus;
		FVector2D C = FVector2D::ZeroVector;
		float Base = 60, Top = 300, Radius = 200;
		TArray<FPuff> Puffs;
		TArray<FTube> Tubes;
		TArray<FPuff> Caves;
		FBox Bounds;
		uint32 Seed = 1;
		float Blend = 14.f;
	};

	void MakeCumulus(FCloud& C, FRandomStream& R)
	{
		const float H = C.Top - C.Base, Rc = C.Radius;
		C.Blend = FMath::Clamp(Rc * 0.08f, 8.f, 22.f);
		const int32 NB = 7 + R.RandRange(0, 3);
		for (int32 I = 0; I < NB; ++I)
		{
			const float A = R.FRand() * 2 * PI, D = Rc * 0.55f * FMath::Sqrt(R.FRand());
			const float RR = Rc * R.FRandRange(0.45f, 0.62f);
			C.Puffs.Add({ FVector(C.C.X + FMath::Cos(A) * D, C.C.Y + FMath::Sin(A) * D, C.Base + RR * 0.32f), FVector(RR, RR, RR * 0.42f) });
		}
		const int32 NT = 6;
		TArray<int32> Tower;
		for (int32 K = 0; K < NT; ++K)
		{
			const float Hk = (K + 0.5f) / NT;
			const float RR = Rc * (0.70f - 0.40f * Hk) * R.FRandRange(0.88f, 1.08f);
			const FVector2D O = FVector2D(R.FRandRange(-1, 1), R.FRandRange(-1, 1)) * Rc * 0.22f * Hk;
			const float Z = C.Base + 0.12f * H + Hk * (H - RR * 0.85f - 0.12f * H);
			Tower.Add(C.Puffs.Add({ FVector(C.C.X + O.X, C.C.Y + O.Y, Z), FVector(RR, RR, RR * 0.86f) }));
		}
		// cauliflower: small lobes on the upper half of the tower
		const int32 NL = 22 + R.RandRange(0, 10);
		for (int32 I = 0; I < NL; ++I)
		{
			const int32 J = Tower[FMath::Min(NT - 1, int32(FMath::Sqrt(R.FRand()) * NT))];
			const FPuff& T = C.Puffs[J];
			FVector D = FVector(R.FRandRange(-1, 1), R.FRandRange(-1, 1), R.FRandRange(-0.2f, 1.f)).GetSafeNormal();
			const float RR = T.R.X * R.FRandRange(0.28f, 0.48f);
			const FVector P = T.P + D * T.R * 0.86f;
			if (P.Z - RR * 0.5f < C.Base) continue;
			C.Puffs.Add({ P, FVector(RR) });
		}
	}

	float CloudShape(const FCloud& C, const FVector& P)
	{
		float S = 1e9f;
		if (C.Kind == EKind::Stratus)
		{
			// flat slab with a noisy outline and a noisy thickness
			const FVector2D Q = FVector2D(P.X, P.Y) - C.C;
			const float Edge = Q.Size() - C.Radius * (0.75f + 0.5f * Fbm(FVector(P.X, P.Y, 0) / 180.f, C.Seed));
			const float Mid = (C.Base + C.Top) * 0.5f, Half = (C.Top - C.Base) * 0.5f;
			const float Th = FMath::Abs(P.Z - Mid) - Half * (0.55f + 0.7f * Fbm(FVector(P.X, P.Y, 7.f) / 90.f, C.Seed + 9));
			S = SMax(Edge, Th, 10.f);
		}
		else
		{
			for (const FPuff& F : C.Puffs) S = SMin(S, SdEllipsoid(P - F.P, F.R), C.Blend);
			S = SMax(S, C.Base - P.Z, 6.f);	// flat cloud base
		}
		// surface displacement: breaks the ellipsoids into irregular lumps
		const float Hf = FMath::Clamp((P.Z - C.Base) / FMath::Max(C.Top - C.Base, 1.f), 0.f, 1.f);
		S += (Fbm(P / 55.f, C.Seed + 3) - 0.5f) * (C.Kind == EKind::Stratus ? 10.f : 18.f) * (0.55f + 0.45f * Hf);
		for (const FTube& T : C.Tubes) S = SMax(S, -(DistToPolyline(P, T.Path) - T.R), 6.f);
		for (const FPuff& V : C.Caves) S = SMax(S, -SdEllipsoid(P - V.P, V.R), 10.f);
		return S;
	}

	// ------------------------------------------------------------ 1D squared distance transform (Felzenszwalb & Huttenlocher)
	void Dt1D(const float* F, float* D, int32 N, int32* V, float* ZF)
	{
		double Z[513];
		int32 K = 0; V[0] = 0; Z[0] = -1e30; Z[1] = 1e30;
		auto Sect = [F](int32 Q, int32 P) { return ((double(F[Q]) + double(Q) * Q) - (double(F[P]) + double(P) * P)) / (2.0 * (Q - P)); };
		for (int32 Q = 1; Q < N; ++Q)
		{
			double S = Sect(Q, V[K]);
			while (S <= Z[K]) { --K; S = Sect(Q, V[K]); }
			++K; V[K] = Q; Z[K] = S; Z[K + 1] = 1e30;
		}
		K = 0;
		for (int32 Q = 0; Q < N; ++Q)
		{
			while (Z[K + 1] < Q) ++K;
			const int32 P = V[K];
			D[Q] = float(double(Q - P) * (Q - P) + F[P]);
		}
	}

	// squared voxel distance to the nearest voxel where Seed is true
	void Edt3(const TArray<uint8>& Seed, bool bSeedValue, TArray<float>& Out)
	{
		const float Inf = 1e12f;
		Out.SetNumUninitialized(NX * NY * NZ);
		for (int32 I = 0; I < Out.Num(); ++I) Out[I] = ((Seed[I] != 0) == bSeedValue) ? 0.f : Inf;
		auto Pass = [&Out](int32 Lines, int32 N, TFunction<int32(int32, int32)> At)
		{
			ParallelFor(Lines, [&](int32 L)
			{
				float F[512], D[512], Z[513]; int32 V[512];
				for (int32 I = 0; I < N; ++I) F[I] = Out[At(L, I)];
				Dt1D(F, D, N, V, Z);
				for (int32 I = 0; I < N; ++I) Out[At(L, I)] = D[I];
			});
		};
		Pass(NY * NZ, NX, [](int32 L, int32 I) { return Idx(I, L % NY, L / NY); });
		Pass(NX * NZ, NY, [](int32 L, int32 I) { return Idx(L % NX, I, L / NX); });
		Pass(NX * NY, NZ, [](int32 L, int32 I) { return Idx(L % NX, L / NX, I); });
	}

	// ------------------------------------------------------------ BC encoders (conservative against every decoder)
	const float U0 = 1.0f, U1 = 0.03529415f, U2 = 0.00069204f;
	int32 E5(int32 X) { return (X << 3) | (X >> 2); }
	int32 E6(int32 X) { return (X << 2) | (X >> 4); }
	float V565(int32 R, int32 G, int32 B) { return E5(R) / 255.f * U0 + E6(G) / 255.f * U1 + E5(B) / 255.f * U2; }
	void RoundDown565(float S, int32& R, int32& G, int32& B)
	{
		R = 0; for (int32 X = 31; X >= 0; --X) if (E5(X) / 255.f * U0 <= S) { R = X; break; }
		float Res = S - E5(R) / 255.f * U0;
		G = 0; for (int32 X = 63; X >= 0; --X) if (E6(X) / 255.f * U1 <= Res) { G = X; break; }
		Res -= E6(G) / 255.f * U1;
		B = 0; for (int32 X = 31; X >= 0; --X) if (E5(X) / 255.f * U2 <= Res) { B = X; break; }
	}
	// Upper bound (in 0..255) of interpolated code 2 (K = 0: 2/3 c0 + 1/3 c1) or code 3 (K = 1) of one channel.
	// Ideal float, AMD ((64-w)a + wb + 32) >> 6, Intel ((256-w)a + wb + 128) >> 8 (F. Giesen, "GPU BCn decoding"),
	// and NVIDIA: 5-bit channels ((2a+b)*22) >> 3, 6-bit green with its own rounding (matches an RTX 3060 read-back).
	float ChanUp(int32 Ai, int32 Bi, bool bSix, int32 K)
	{
		const int32 A = bSix ? E6(Ai) : E5(Ai), B = bSix ? E6(Bi) : E5(Bi);
		const float Ideal = K == 0 ? (2.f * A + B) / 3.f : (A + 2.f * B) / 3.f;
		const int32 Amd = K == 0 ? (43 * A + 21 * B + 32) >> 6 : (21 * A + 43 * B + 32) >> 6;
		const int32 Intel = K == 0 ? (171 * A + 85 * B + 128) >> 8 : (85 * A + 171 * B + 128) >> 8;
		int32 Nv;
		if (!bSix) Nv = K == 0 ? ((2 * Ai + Bi) * 22) >> 3 : ((2 * Bi + Ai) * 22) >> 3;
		else
		{
			const int32 Diff = B - A, Scaled = 80 * Diff + (Diff >> 2);
			Nv = K == 0 ? A + ((128 + Scaled) >> 8) : B + ((128 - Scaled) >> 8);
		}
		return FMath::Max(FMath::Max(Ideal, (float)Amd), (float)FMath::Max(Intel, Nv));
	}

	void EncodeBC1Block(const float* V, uint8* Out, bool bSafe, float* Dec)
	{
		float Mn = 1e9f, Mx = -1e9f;
		for (int32 I = 0; I < 16; ++I) { Mn = FMath::Min(Mn, V[I]); Mx = FMath::Max(Mx, V[I]); }
		int32 LR, LG, LB; RoundDown565(Mn, LR, LG, LB);
		const uint16 C1 = uint16((LR << 11) | (LG << 5) | LB);
		float BestErr = 1e9f; uint16 BestC0 = C1; uint32 BestBits = 0x55555555u; float BestDec[16];
		for (int32 I = 0; I < 16; ++I) BestDec[I] = V565(LR, LG, LB);
		for (int32 K = 0; K < 16; ++K)
		{
			const float Target = Mx - (Mx - Mn) * (K / 24.f);
			int32 HR, HG, HB; RoundDown565(Target, HR, HG, HB);
			const uint16 C0 = uint16((HR << 11) | (HG << 5) | HB);
			if (C0 <= C1) continue;
			float Pal[4];
			Pal[0] = V565(HR, HG, HB); Pal[1] = V565(LR, LG, LB);
			for (int32 Q = 0; Q < 2; ++Q)
			{
				if (bSafe) Pal[2 + Q] = ChanUp(HR, LR, false, Q) / 255.f * U0 + ChanUp(HG, LG, true, Q) / 255.f * U1 + ChanUp(HB, LB, false, Q) / 255.f * U2;
				else Pal[2 + Q] = Q == 0 ? (2 * Pal[0] + Pal[1]) / 3.f : (Pal[0] + 2 * Pal[1]) / 3.f;
			}
			float Err = 0; uint32 Bits = 0; float D[16];
			for (int32 I = 0; I < 16; ++I)
			{
				int32 Pick = 1;
				if (bSafe)
				{
					float Best = -1;
					for (int32 C = 0; C < 4; ++C) if (Pal[C] <= V[I] + 1e-7f && Pal[C] > Best) { Best = Pal[C]; Pick = C; }
				}
				else
				{
					float Best = 1e9f;
					for (int32 C = 0; C < 4; ++C) if (FMath::Abs(Pal[C] - V[I]) < Best) { Best = FMath::Abs(Pal[C] - V[I]); Pick = C; }
				}
				D[I] = Pal[Pick];
				Err += FMath::Abs(V[I] - Pal[Pick]);
				Bits |= uint32(Pick) << (2 * I);
			}
			if (Err < BestErr) { BestErr = Err; BestC0 = C0; BestBits = Bits; FMemory::Memcpy(BestDec, D, sizeof(D)); }
		}
		Out[0] = BestC0 & 255; Out[1] = BestC0 >> 8; Out[2] = C1 & 255; Out[3] = C1 >> 8;
		for (int32 J = 0; J < 4; ++J) Out[4 + J] = (BestBits >> (8 * J)) & 255;
		FMemory::Memcpy(Dec, BestDec, sizeof(BestDec));
	}

	void EncodeBC4Block(const float* V, uint8* Out, float* Dec)
	{
		float Mn = 1e9f, Mx = -1e9f;
		for (int32 I = 0; I < 16; ++I) { Mn = FMath::Min(Mn, V[I]); Mx = FMath::Max(Mx, V[I]); }
		const int32 E1 = FMath::Clamp(FMath::FloorToInt(Mn * 255.f), 0, 254);
		const int32 Hi0 = FMath::Clamp(FMath::CeilToInt(Mx * 255.f), 0, 255);
		float BestErr = 1e9f; int32 BE0 = E1 + 1, BE1 = E1; uint64 BestBits = 0; float BestDec[16];
		for (int32 K = 0; K < 24; ++K)
		{
			const int32 E0 = FMath::Min(255, FMath::Max(Hi0 - K, E1 + 1));
			const int32 E1c = FMath::Min(E1, E0 - 1);
			float Pal[8];
			for (int32 S = 0; S < 8; ++S)
			{
				if (S == 0) { Pal[S] = E0 / 255.f; continue; }
				if (S == 7) { Pal[S] = E1c / 255.f; continue; }
				const float Ideal = ((7 - S) * E0 + S * E1c) / 7.f / 255.f;
				const int32 Wa = FMath::RoundToInt(S * 64.f / 7.f);
				const float Amd = ((64 - Wa) * E0 + Wa * E1c) / 16320.f;
				const int64 Wi = FMath::RoundToInt(S * 65536.0 / 7.0);
				const int64 Ti = ((65536 - Wi) * E0 + Wi * E1c + 128) >> 8;
				const float Intel = (Ti + (Ti >> 8)) / 65535.f;
				const float Nv = Ideal + FMath::Abs(E0 - E1c) * 0.0112f / 255.f + 1.f / 65535.f;	// RTX 3060 read-back: |error| <= 0.0111 * |e0 - e1| / 255
				Pal[S] = FMath::Max(FMath::Max(Ideal, Amd), FMath::Max(Intel, Nv));
			}
			float Err = 0; uint64 Bits = 0; float D[16];
			for (int32 I = 0; I < 16; ++I)
			{
				int32 Pick = 7; float Best = -1;
				for (int32 S = 0; S < 8; ++S) if (Pal[S] <= V[I] + 1e-7f && Pal[S] > Best) { Best = Pal[S]; Pick = S; }
				D[I] = Pal[Pick];
				Err += V[I] - Pal[Pick];
				const uint64 Code = Pick == 0 ? 0 : (Pick == 7 ? 1 : Pick + 1);
				Bits |= Code << (3 * I);
			}
			if (Err < BestErr) { BestErr = Err; BE0 = E0; BE1 = E1c; BestBits = Bits; FMemory::Memcpy(BestDec, D, sizeof(D)); }
		}
		Out[0] = (uint8)BE0; Out[1] = (uint8)BE1;
		for (int32 J = 0; J < 6; ++J) Out[2 + J] = (BestBits >> (8 * J)) & 255;
		FMemory::Memcpy(Dec, BestDec, sizeof(BestDec));
	}

	float ToLog(float S) { return FMath::Loge(1.f + (S - SdfLo) / 64.f) / FMath::Loge(1.f + (SdfHi - SdfLo) / 64.f); }
	float FromLog(float U) { return SdfLo + 64.f * (FMath::Exp(U * FMath::Loge(1.f + (SdfHi - SdfLo) / 64.f)) - 1.f); }
}

// ================================================================ build

void FCLCloudField::Build(int32 Seed)
{
	const int32 N = NX * NY * NZ;
	TArray<float> S;
	TArray<uint8> Type, DScale;
	double T0 = FPlatformTime::Seconds();
	Shape(Seed, S, Type, DScale);
	Stats.ShapeMs = (FPlatformTime::Seconds() - T0) * 1000.0;

	// modelling NVDF: profile from the shape's distance (gradient from the surface inward), type, density scale, zone
	TArray<uint8>& Model = Data[0].Modeling;
	Model.SetNumUninitialized(N * 4);
	TArray<uint8> Occ; Occ.SetNumZeroed(N);
	int64 Filled = 0;
	for (int32 I = 0; I < N; ++I)
	{
		const float P = S[I] < 0.f ? FMath::Pow(FMath::Clamp(-S[I] / 40.f, 0.f, 1.f), 0.7f) : 0.f;
		const uint8 B = (uint8)FMath::Clamp(FMath::RoundToInt(P * 255.f), 0, 255);
		Model[I * 4 + 0] = B;
		Model[I * 4 + 1] = Type[I];
		Model[I * 4 + 2] = DScale[I];
		Model[I * 4 + 3] = Zone[I];
		Occ[I] = B > 0 ? 1 : 0;
		Filled += Occ[I];
	}
	Stats.Occupancy = float(Filled) / N;

	T0 = FPlatformTime::Seconds();
	SignedEdt(Occ, SdfM[0]);
	Stats.EdtMs = (FPlatformTime::Seconds() - T0) * 1000.0;

	T0 = FPlatformTime::Seconds();
	Envelope(Model, Data[1].Modeling);
	TArray<uint8> EnvOcc; EnvOcc.SetNumZeroed(N);
	int64 EnvFilled = 0;
	for (int32 I = 0; I < N; ++I) { EnvOcc[I] = Data[1].Modeling[I * 4] > 0 ? 1 : 0; EnvFilled += EnvOcc[I]; }
	Stats.EnvelopeOccupancy = float(EnvFilled) / N;
	SignedEdt(EnvOcc, SdfM[1]);
	Stats.EnvelopeMs = (FPlatformTime::Seconds() - T0) * 1000.0;

	T0 = FPlatformTime::Seconds();
	Encode(SdfM[0], Data[0], Stats, true);
	Encode(SdfM[1], Data[1], Stats, false);
	Stats.EncodeMs = (FPlatformTime::Seconds() - T0) * 1000.0;

	T0 = FPlatformTime::Seconds();
	MakeNoise();
	Stats.NoiseMs = (FPlatformTime::Seconds() - T0) * 1000.0;

	// how much the envelope fills the first tunnel (its centre line is free air in the voxel version)
	if (Course.Num() > 2) Stats.FilledTunnel = Density(Course[1].PosM, ECLRepresentation::Envelope);
}

void FCLCloudField::Shape(int32 Seed, TArray<float>& S, TArray<uint8>& Type, TArray<uint8>& DScale)
{
	const int32 N = NX * NY * NZ;
	S.Init(1e9f, N);
	TArray<uint8> Owner; Owner.Init(255, N);
	FRandomStream R(Seed);
	TArray<FCloud> Clouds;
	auto Cum = [&](FVector2D C, float Base, float Top, float Rad) -> FCloud&
	{
		FCloud& K = Clouds.AddDefaulted_GetRef();
		K.Kind = EKind::Cumulus; K.C = C; K.Base = Base; K.Top = Top; K.Radius = Rad; K.Seed = R.RandRange(1, 1 << 20);
		MakeCumulus(K, R);
		return K;
	};

	// ---------------- the course: start -> tunnel cloud -> slalom -> cave cloud -> under the stratus deck -> storm lair -> finish
	Course.Reset();
	auto Ring = [this](FVector P, FVector D, float Rr, const TCHAR* Label) { Course.Add({ P, D.GetSafeNormal(), Rr, Label }); };
	StartM = FVector(380, 620, 118); StartDir = FVector(1, 0.62f, 0.02f).GetSafeNormal();

	Ring(FVector(700, 820, 128), FVector(1, 0.62f, 0.02f), 18.f, TEXT("热身"));
	// 1. tunnel cloud: a long straight-ish tunnel through a big cumulus
	{
		FCloud& A = Cum(FVector2D(1160, 1150), 70, 360, 250);
		FTube T; T.R = 26.f;
		T.Path = { FVector(840, 975, 150), FVector(1000, 1065, 158), FVector(1160, 1152, 166), FVector(1320, 1245, 162), FVector(1480, 1335, 156) };
		A.Tubes.Add(T);
		Ring(FVector(920, 1020, 154), FVector(1, 0.56f, 0.05f), 15.f, TEXT("隧道入口"));
		Ring(FVector(1160, 1152, 166), FVector(1, 0.56f, 0.0f), 15.f, TEXT("隧道中段"));
		Ring(FVector(1400, 1290, 159), FVector(1, 0.56f, -0.04f), 15.f, TEXT("隧道出口"));
	}
	// 2. slalom between two towers
	Cum(FVector2D(1760, 1330), 55, 300, 165);
	Cum(FVector2D(2010, 1790), 80, 380, 195);
	Ring(FVector(1830, 1580, 140), FVector(0.85f, 0.75f, 0.0f), 18.f, TEXT("云间"));
	// 3. cave cloud: an entrance tunnel, a chamber, a climbing exit
	{
		FCloud& E = Cum(FVector2D(2600, 2160), 50, 430, 300);
		E.Caves.Add({ FVector(2600, 2160, 195), FVector(92, 92, 62) });
		FTube In; In.R = 28.f; In.Path = { FVector(2280, 1985, 170), FVector(2440, 2075, 182), FVector(2560, 2140, 192) };
		FTube Out; Out.R = 28.f; Out.Path = { FVector(2640, 2185, 200), FVector(2760, 2265, 218), FVector(2920, 2360, 240) };
		E.Tubes.Add(In); E.Tubes.Add(Out);
		Ring(FVector(2380, 2042, 177), FVector(1, 0.56f, 0.07f), 16.f, TEXT("云洞入口"));
		Ring(FVector(2600, 2160, 196), FVector(1, 0.6f, 0.05f), 20.f, TEXT("云洞"));
		Ring(FVector(2840, 2312, 229), FVector(1, 0.6f, 0.13f), 16.f, TEXT("云洞出口"));
	}
	// 4. stratus deck: fly under its base
	{
		FCloud& St = Clouds.AddDefaulted_GetRef();
		St.Kind = EKind::Stratus; St.C = FVector2D(3020, 2700); St.Base = 280; St.Top = 330; St.Radius = 420; St.Seed = R.RandRange(1, 1 << 20);
		Ring(FVector(3030, 2690, 248), FVector(0.62f, 1, -0.02f), 18.f, TEXT("云底"));
	}
	// 5. storm cloud with a lair inside, lit by lightning
	{
		FCloud& F = Clouds.AddDefaulted_GetRef();
		F.Kind = EKind::Storm; F.C = FVector2D(3320, 3260); F.Base = 25; F.Top = 505; F.Radius = 360; F.Seed = R.RandRange(1, 1 << 20);
		F.Blend = 26.f;
		for (int32 I = 0; I < 9; ++I)
		{
			const float A = I / 9.f * 2 * PI + R.FRand() * 0.4f, D = 200.f * FMath::Sqrt(R.FRand());
			F.Puffs.Add({ FVector(F.C.X + FMath::Cos(A) * D, F.C.Y + FMath::Sin(A) * D, 70), FVector(230, 230, 95) });
		}
		for (int32 K = 0; K < 7; ++K)
		{
			const float Hk = (K + 0.5f) / 7.f, RR = 300.f - 110.f * Hk;
			F.Puffs.Add({ FVector(F.C.X + R.FRandRange(-40, 40), F.C.Y + R.FRandRange(-40, 40), 90 + Hk * 330), FVector(RR, RR, RR * 0.8f) });
		}
		F.Puffs.Add({ FVector(F.C.X + 60, F.C.Y + 40, 468), FVector(560, 520, 44) });	// anvil
		for (int32 I = 0; I < 30; ++I)
		{
			const FVector D = FVector(R.FRandRange(-1, 1), R.FRandRange(-1, 1), R.FRandRange(-0.1f, 1.f)).GetSafeNormal();
			const float RR = R.FRandRange(60, 110);
			const FVector P = FVector(F.C.X, F.C.Y, 260) + D * FVector(250, 250, 200);
			F.Puffs.Add({ P, FVector(RR) });
		}
		const FVector Lair(3320, 3260, 215);
		F.Caves.Add({ Lair, FVector(125, 125, 80) });
		F.Caves.Add({ Lair + FVector(-60, 70, -30), FVector(70, 60, 50) });
		F.Caves.Add({ Lair + FVector(80, -50, 25), FVector(60, 70, 45) });
		FTube In; In.R = 30.f; In.Path = { FVector(3050, 2955, 205), FVector(3160, 3080, 210), FVector(3250, 3180, 214) };
		FTube Out; Out.R = 30.f; Out.Path = { FVector(3390, 3345, 225), FVector(3480, 3450, 262), FVector(3600, 3590, 300) };
		F.Tubes.Add(In); F.Tubes.Add(Out);
		StormCoreM = Lair; StormRadiusM = 420.f;
		Ring(FVector(3110, 3022, 207), FVector(0.66f, 0.75f, 0.03f), 17.f, TEXT("风暴入口"));
		Ring(Lair, FVector(0.66f, 0.75f, 0.06f), 22.f, TEXT("风暴巢穴"));
		Ring(FVector(3520, 3500, 275), FVector(0.66f, 0.75f, 0.3f), 17.f, TEXT("风暴出口"));
		Ring(FVector(3720, 3740, 300), FVector(0.66f, 0.75f, 0.0f), 22.f, TEXT("终点"));
	}
	// ---------------- background clouds that keep clear of the course corridor
	TArray<FVector> Path; Path.Add(StartM); for (const FCLRingSpot& Sp : Course) Path.Add(Sp.PosM);
	int32 Placed = 0;
	for (int32 Try = 0; Try < 400 && Placed < 34; ++Try)
	{
		const FVector2D C(R.FRandRange(150, 3950), R.FRandRange(150, 3950));
		const float Rad = R.FRandRange(80, 230);
		const float Clear = DistToPolyline(FVector(C.X, C.Y, 160), Path);
		bool bOk = Clear > Rad * 1.7f + 110.f;
		for (const FCloud& K : Clouds) if (FVector2D::Distance(K.C, C) < (K.Radius + Rad) * 0.9f) bOk = false;
		if (!bOk) continue;
		const float Base = R.FRandRange(40, 140);
		Cum(C, Base, FMath::Min(Base + Rad * R.FRandRange(1.0f, 1.7f), 500.f), Rad);
		++Placed;
	}
	{
		FCloud& St = Clouds.AddDefaulted_GetRef();
		St.Kind = EKind::Stratus; St.C = FVector2D(900, 3100); St.Base = 330; St.Top = 370; St.Radius = 520; St.Seed = R.RandRange(1, 1 << 20);
	}

	// bounds in voxels
	for (FCloud& K : Clouds)
	{
		FBox B(ForceInit);
		if (K.Kind == EKind::Stratus) B = FBox(FVector(K.C.X - K.Radius * 1.35f, K.C.Y - K.Radius * 1.35f, K.Base - 30), FVector(K.C.X + K.Radius * 1.35f, K.C.Y + K.Radius * 1.35f, K.Top + 30));
		else for (const FPuff& F : K.Puffs) B += FBox(F.P - F.R - FVector(30), F.P + F.R + FVector(30));
		K.Bounds = B;
	}

	// evaluate each cloud inside its own box; the cloudscape is the union (minimum) of all of them
	for (int32 Ci = 0; Ci < Clouds.Num(); ++Ci)
	{
		const FCloud& K = Clouds[Ci];
		const FIntVector Lo(FMath::Clamp(FMath::FloorToInt(K.Bounds.Min.X / VoxelM), 0, NX - 1), FMath::Clamp(FMath::FloorToInt(K.Bounds.Min.Y / VoxelM), 0, NY - 1), FMath::Clamp(FMath::FloorToInt(K.Bounds.Min.Z / VoxelM), 0, NZ - 1));
		const FIntVector Hi(FMath::Clamp(FMath::CeilToInt(K.Bounds.Max.X / VoxelM), 0, NX - 1), FMath::Clamp(FMath::CeilToInt(K.Bounds.Max.Y / VoxelM), 0, NY - 1), FMath::Clamp(FMath::CeilToInt(K.Bounds.Max.Z / VoxelM), 0, NZ - 1));
		ParallelFor(Hi.Z - Lo.Z + 1, [&](int32 Dz)
		{
			const int32 Z = Lo.Z + Dz;
			for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
				for (int32 X = Lo.X; X <= Hi.X; ++X)
				{
					const FVector P((X + 0.5f) * VoxelM, (Y + 0.5f) * VoxelM, (Z + 0.5f) * VoxelM);
					const float V = CloudShape(K, P);
					const int32 I = Idx(X, Y, Z);
					if (V < S[I]) { S[I] = V; Owner[I] = (uint8)FMath::Min(Ci, 254); }
				}
		});
	}

	// detail type and density scale from the owning cloud
	Type.SetNumUninitialized(N); DScale.SetNumUninitialized(N); Zone.SetNumZeroed(N);
	ParallelFor(NZ, [&](int32 Z)
	{
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
			{
				const int32 I = Idx(X, Y, Z);
				float Tp = 0.5f, Ds = 1.f;
				if (Owner[I] != 255)
				{
					const FCloud& K = Clouds[Owner[I]];
					const FVector P((X + 0.5f) * VoxelM, (Y + 0.5f) * VoxelM, (Z + 0.5f) * VoxelM);
					const float Hf = FMath::Clamp((P.Z - K.Base) / FMath::Max(K.Top - K.Base, 1.f), 0.f, 1.f);
					const float Nn = ValueNoise(P / 70.f, K.Seed + 17);
					switch (K.Kind)
					{
					case EKind::Cumulus: Tp = 0.2f + 0.85f * Hf + 0.3f * (Nn - 0.5f); Ds = 0.9f + 0.1f * Nn; break;
					case EKind::Stratus: Tp = 0.1f + 0.2f * Nn; Ds = 0.72f; break;
					case EKind::Storm: Tp = 0.75f + 0.25f * Hf; Ds = 1.f; Zone[I] = 255; break;
					}
					Ds *= FMath::GetMappedRangeValueClamped(FVector2D(0, 30), FVector2D(0.78f, 1.f), P.Z - K.Base);
				}
				Type[I] = (uint8)FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Tp, 0.f, 1.f) * 255.f), 0, 255);
				DScale[I] = (uint8)FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Ds, 0.f, 1.f) * 255.f), 0, 255);
			}
	});
}

void FCLCloudField::Envelope(const TArray<uint8>& Model, TArray<uint8>& Env)
{
	// The 2.5D envelope of the same cloudscape: per column the lowest and highest cloud voxel, a coverage map and
	// an edge gradient from the coverage outline (the talk's Envelope Method, min/max height + edge gradient).
	const int32 N = NX * NY * NZ;
	Env.SetNumZeroed(N * 4);
	TArray<int32> ZMin, ZMax; ZMin.Init(-1, NX * NY); ZMax.Init(-1, NX * NY);
	TArray<float> TypeSum, DMax; TypeSum.SetNumZeroed(NX * NY); DMax.SetNumZeroed(NX * NY);
	TArray<int32> Count; Count.SetNumZeroed(NX * NY);
	for (int32 Y = 0; Y < NY; ++Y)
		for (int32 X = 0; X < NX; ++X)
		{
			const int32 C = Y * NX + X;
			for (int32 Z = 0; Z < NZ; ++Z)
			{
				const int32 I = Idx(X, Y, Z) * 4;
				if (Model[I] == 0) continue;
				if (ZMin[C] < 0) ZMin[C] = Z;
				ZMax[C] = Z;
				TypeSum[C] += Model[I + 1]; ++Count[C];
				DMax[C] = FMath::Max(DMax[C], (float)Model[I + 2]);
			}
		}
	// 2D distance (m) from each covered column to the nearest uncovered one
	TArray<float> Edge; Edge.SetNumUninitialized(NX * NY);
	{
		TArray<float> G; G.SetNumUninitialized(NX * NY);
		for (int32 C = 0; C < NX * NY; ++C) G[C] = ZMin[C] < 0 ? 0.f : 1e12f;
		ParallelFor(NY, [&](int32 Y) { float F[512], D[512], Z[513]; int32 V[512]; for (int32 X = 0; X < NX; ++X) F[X] = G[Y * NX + X]; Dt1D(F, D, NX, V, Z); for (int32 X = 0; X < NX; ++X) G[Y * NX + X] = D[X]; });
		ParallelFor(NX, [&](int32 X) { float F[512], D[512], Z[513]; int32 V[512]; for (int32 Y = 0; Y < NY; ++Y) F[Y] = G[Y * NX + X]; Dt1D(F, D, NY, V, Z); for (int32 Y = 0; Y < NY; ++Y) G[Y * NX + X] = D[Y]; });
		for (int32 C = 0; C < NX * NY; ++C) Edge[C] = FMath::Sqrt(G[C]) * VoxelM;
	}
	ParallelFor(NY, [&](int32 Y)
	{
		for (int32 X = 0; X < NX; ++X)
		{
			const int32 C = Y * NX + X;
			if (ZMin[C] < 0) continue;
			const float Z0 = ZMin[C], Z1 = ZMax[C] + 1.f;
			const float EdgeG = FMath::Clamp(Edge[C] / 40.f, 0.f, 1.f);
			const uint8 Tp = (uint8)FMath::Clamp(FMath::RoundToInt(TypeSum[C] / FMath::Max(Count[C], 1)), 0, 255);
			for (int32 Z = ZMin[C]; Z <= ZMax[C]; ++Z)
			{
				const float Hf = ((Z + 0.5f) - Z0) / FMath::Max(Z1 - Z0, 1.f);
				const float Bottom = FMath::Clamp(Hf / 0.15f + 0.08f, 0.f, 1.f), Top = FMath::Clamp((1.f - Hf) / 0.35f + 0.05f, 0.f, 1.f);
				const float P = FMath::Pow(FMath::Clamp(Bottom * Top * EdgeG, 0.f, 1.f), 0.7f);
				const int32 I = Idx(X, Y, Z) * 4;
				Env[I] = (uint8)FMath::Clamp(FMath::RoundToInt(P * 255.f), 1, 255);
				Env[I + 1] = Tp;
				Env[I + 2] = (uint8)DMax[C];
				Env[I + 3] = Model[Idx(X, Y, ZMax[C]) * 4 + 3];
			}
		}
	});
}

void FCLCloudField::SignedEdt(const TArray<uint8>& Occ, TArray<float>& Out)
{
	TArray<float> Outside, Inside;
	Edt3(Occ, true, Outside);		// squared voxel distance to the nearest cloud voxel
	Edt3(Occ, false, Inside);		// ... to the nearest empty voxel
	const int32 N = Occ.Num();
	Out.SetNumUninitialized(N);
	ParallelFor(NZ, [&](int32 Z)
	{
		for (int32 I = Z * NX * NY, E = (Z + 1) * NX * NY; I < E; ++I)
		{
			const float D = Occ[I] ? -(FMath::Sqrt(Inside[I]) - 0.5f) * VoxelM : (FMath::Sqrt(Outside[I]) - 0.5f) * VoxelM;
			Out[I] = FMath::Clamp(D, SdfLo, SdfHi);
		}
	});
}

void FCLCloudField::Encode(const TArray<float>& Sdf, FCLVolumeData& D, FStats& St, bool bStats)
{
	const int32 N = NX * NY * NZ;
	D.SdfR16.SetNumUninitialized(N);
	for (int32 I = 0; I < N; ++I)
		D.SdfR16[I] = (uint16)FMath::Clamp(FMath::FloorToInt((Sdf[I] - SdfLo) / (SdfHi - SdfLo) * 65535.f), 0, 65535);
	const int32 BX = NX / 4, BY = NY / 4, NB = BX * BY * NZ;
	D.SdfBC1Safe.SetNumUninitialized(NB * 8);
	D.SdfBC1Naive.SetNumUninitialized(NB * 8);
	D.SdfBC4Log.SetNumUninitialized(NB * 8);
	TArray<double> Under1, Under4; Under1.SetNumZeroed(NZ); Under4.SetNumZeroed(NZ);
	TArray<int64> Over1, OverN, Over4; Over1.SetNumZeroed(NZ); OverN.SetNumZeroed(NZ); Over4.SetNumZeroed(NZ);
	ParallelFor(NZ, [&](int32 Z)
	{
		for (int32 By = 0; By < BY; ++By)
			for (int32 Bx = 0; Bx < BX; ++Bx)
			{
				float V[16], Vl[16], M[16], Dec[16];
				for (int32 T = 0; T < 16; ++T)
				{
					const float Sm = Sdf[Idx(Bx * 4 + (T & 3), By * 4 + (T >> 2), Z)];
					M[T] = Sm;
					V[T] = (Sm - SdfLo) / (SdfHi - SdfLo);
					Vl[T] = ToLog(Sm);
				}
				const int32 B = (Z * BY + By) * BX + Bx;
				EncodeBC1Block(V, &D.SdfBC1Safe[B * 8], true, Dec);
				for (int32 T = 0; T < 16; ++T) { const float Dm = SdfLo + Dec[T] * (SdfHi - SdfLo); Under1[Z] += M[T] - Dm; if (Dm > M[T] + 1e-3f) ++Over1[Z]; }
				EncodeBC1Block(V, &D.SdfBC1Naive[B * 8], false, Dec);
				for (int32 T = 0; T < 16; ++T) if (SdfLo + Dec[T] * (SdfHi - SdfLo) > M[T] + 1e-3f) ++OverN[Z];
				EncodeBC4Block(Vl, &D.SdfBC4Log[B * 8], Dec);
				for (int32 T = 0; T < 16; ++T) { const float Dm = FromLog(Dec[T]); Under4[Z] += M[T] - Dm; if (Dm > M[T] + 1e-2f) ++Over4[Z]; }
			}
	});
	if (bStats)
	{
		double SumU1 = 0, SumU4 = 0; int64 O1 = 0, ON = 0, O4 = 0;
		for (int32 Z = 0; Z < NZ; ++Z) { SumU1 += Under1[Z]; SumU4 += Under4[Z]; O1 += Over1[Z]; ON += OverN[Z]; O4 += Over4[Z]; }
		St.BC1SafeMeanUnder = SumU1 / N; St.BC4MeanUnder = SumU4 / N;
		St.BC1SafeOver = O1; St.BC1NaiveOver = ON; St.BC4Over = O4;
	}
}

void FCLCloudField::MakeNoise()
{
	// 128^3, tileable. R/G: low/high frequency "wispy" noise = layered Worley F1 of a periodically warped position
	// (a stand-in for the talk's curl-distorted inverted Alligator). B/A: low/high frequency "billowy" noise =
	// 1 - layered Worley F1 (a stand-in for Alligator noise). Each channel is stretched to the full 0..1 range.
	const int32 S = NoiseN;
	TArray<float> Ch[4];
	for (int32 C = 0; C < 4; ++C) Ch[C].SetNumUninitialized(S * S * S);
	auto Worley = [](const FVector& P, int32 Cells, uint32 Seed)
	{
		const FVector Q = P * Cells;
		const int32 X0 = FMath::FloorToInt(Q.X), Y0 = FMath::FloorToInt(Q.Y), Z0 = FMath::FloorToInt(Q.Z);
		float Best = 9.f;
		for (int32 Z = -1; Z <= 1; ++Z) for (int32 Y = -1; Y <= 1; ++Y) for (int32 X = -1; X <= 1; ++X)
		{
			const int32 CX = X0 + X, CY = Y0 + Y, CZ = Z0 + Z;
			const int32 WX = (CX % Cells + Cells) % Cells, WY = (CY % Cells + Cells) % Cells, WZ = (CZ % Cells + Cells) % Cells;
			const uint32 H = Hash3(WX, WY, WZ, Seed);
			const FVector F(CX + Rand01(H), CY + Rand01(H * 747796405u + 1u), CZ + Rand01(H * 2891336453u + 7u));
			Best = FMath::Min(Best, (float)FVector::DistSquared(F, Q));
		}
		return FMath::Min(FMath::Sqrt(Best), 1.f);
	};
	ParallelFor(S, [&](int32 Z)
	{
		for (int32 Y = 0; Y < S; ++Y)
			for (int32 X = 0; X < S; ++X)
			{
				const FVector U = (FVector(X, Y, Z) + 0.5f) / float(S);
				const float TwoPi = 2.f * PI;
				const FVector W1 = FVector(FMath::Sin(TwoPi * (2 * U.Y + U.Z)), FMath::Sin(TwoPi * (2 * U.Z + U.X)), FMath::Sin(TwoPi * (2 * U.X + U.Y))) * 0.06f;
				const FVector W2 = FVector(FMath::Sin(TwoPi * (5 * U.Y + 2 * U.Z)), FMath::Sin(TwoPi * (5 * U.Z + 2 * U.X)), FMath::Sin(TwoPi * (5 * U.X + 2 * U.Y))) * 0.03f;
				const FVector A1 = U + W1, A2 = U + W1 * 0.6f + W2;
				const int32 I = (Z * S + Y) * S + X;
				Ch[0][I] = 0.625f * Worley(A1, 5, 11) + 0.25f * Worley(A1, 10, 12) + 0.125f * Worley(A1, 20, 13);
				Ch[1][I] = 0.625f * Worley(A2, 12, 21) + 0.25f * Worley(A2, 24, 22) + 0.125f * Worley(A2, 48, 23);
				Ch[2][I] = 1.f - (0.625f * Worley(U, 4, 31) + 0.25f * Worley(U, 8, 32) + 0.125f * Worley(U, 16, 33));
				Ch[3][I] = 1.f - (0.625f * Worley(U, 10, 41) + 0.25f * Worley(U, 20, 42) + 0.125f * Worley(U, 40, 43));
			}
	});
	NoiseMips.Reset();
	TArray<uint8>& M0 = NoiseMips.AddDefaulted_GetRef();
	M0.SetNumUninitialized(S * S * S * 4);
	for (int32 C = 0; C < 4; ++C)
	{
		float Mn = 1e9f, Mx = -1e9f;
		for (float V : Ch[C]) { Mn = FMath::Min(Mn, V); Mx = FMath::Max(Mx, V); }
		const float Inv = 1.f / FMath::Max(Mx - Mn, 1e-4f);
		for (int32 I = 0; I < Ch[C].Num(); ++I) M0[I * 4 + C] = (uint8)FMath::Clamp(FMath::RoundToInt((Ch[C][I] - Mn) * Inv * 255.f), 0, 255);
	}
	for (int32 W = S; W > 1; W /= 2)
	{
		const int32 H = W / 2;
		const TArray<uint8> Src = NoiseMips.Last();
		TArray<uint8> Dst; Dst.SetNumUninitialized(H * H * H * 4);
		for (int32 Z = 0; Z < H; ++Z) for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < H; ++X) for (int32 C = 0; C < 4; ++C)
		{
			int32 Acc = 0;
			for (int32 K = 0; K < 8; ++K) Acc += Src[(((2 * Z + (K >> 2)) * W + (2 * Y + ((K >> 1) & 1))) * W + (2 * X + (K & 1))) * 4 + C];
			Dst[((Z * H + Y) * H + X) * 4 + C] = (uint8)((Acc + 4) / 8);
		}
		NoiseMips.Add(MoveTemp(Dst));
	}
}

// ================================================================ queries

float FCLCloudField::Density(const FVector& PM, ECLRepresentation Rep) const
{
	const TArray<uint8>& M = Data[(int32)Rep].Modeling;
	if (M.Num() == 0) return 0.f;
	const FVector G = PM / VoxelM - FVector(0.5f);
	const int32 X0 = FMath::FloorToInt(G.X), Y0 = FMath::FloorToInt(G.Y), Z0 = FMath::FloorToInt(G.Z);
	const FVector F = G - FVector(X0, Y0, Z0);
	float Acc = 0;
	for (int32 K = 0; K < 8; ++K)
	{
		const int32 X = FMath::Clamp(X0 + (K & 1), 0, NX - 1), Y = FMath::Clamp(Y0 + ((K >> 1) & 1), 0, NY - 1), Z = FMath::Clamp(Z0 + (K >> 2), 0, NZ - 1);
		const float W = ((K & 1) ? F.X : 1 - F.X) * (((K >> 1) & 1) ? F.Y : 1 - F.Y) * ((K >> 2) ? F.Z : 1 - F.Z);
		const int32 I = Idx(X, Y, Z) * 4;
		Acc += W * (M[I] / 255.f) * (M[I + 2] / 255.f);
	}
	if (PM.Z < 0 || PM.Z > NZ * VoxelM || PM.X < 0 || PM.Y < 0 || PM.X > NX * VoxelM || PM.Y > NY * VoxelM) return 0.f;
	return Acc;
}

float FCLCloudField::Sdf(const FVector& PM, ECLRepresentation Rep) const
{
	const TArray<float>& S = SdfM[(int32)Rep];
	if (S.Num() == 0) return 1e4f;
	const FVector G = PM / VoxelM - FVector(0.5f);
	const int32 X0 = FMath::FloorToInt(G.X), Y0 = FMath::FloorToInt(G.Y), Z0 = FMath::FloorToInt(G.Z);
	const FVector F = G - FVector(X0, Y0, Z0);
	float Acc = 0;
	for (int32 K = 0; K < 8; ++K)
	{
		const int32 X = FMath::Clamp(X0 + (K & 1), 0, NX - 1), Y = FMath::Clamp(Y0 + ((K >> 1) & 1), 0, NY - 1), Z = FMath::Clamp(Z0 + (K >> 2), 0, NZ - 1);
		const float W = ((K & 1) ? F.X : 1 - F.X) * (((K >> 1) & 1) ? F.Y : 1 - F.Y) * ((K >> 2) ? F.Z : 1 - F.Z);
		Acc += W * S[Idx(X, Y, Z)];
	}
	return Acc;
}

bool FCLCloudField::InStorm(const FVector& PM) const
{
	return StormRadiusM > 0 && FVector::Dist(PM, StormCoreM) < StormRadiusM;
}
