// Scripted self-test (-CLCapture=<dir>): places the plane at fixed spots, flips the principle switches, takes
// screenshots, logs GPU times and PASS/FAIL lines to <dir>/results.txt, flies the course on autopilot, then quits.

#include "CLCloudField.h"
#include "CLCloudManager.h"
#include "CLPlane.h"
#include "CLPlayerController.h"
#include "CLRace.h"
#include "CLWorld.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

namespace
{
	struct FStep { float At; TFunction<void(ACLPlayerController*)> Do; };
}

void ACLPlayerController::Shot(const FString& Name)
{
	FScreenshotRequest::RequestScreenshot(FPaths::Combine(CaptureDir, Name + TEXT(".png")), true, false);
}

void ACLPlayerController::Check(bool bOk, const FString& Name, const FString& Detail)
{
	const FString Line = FString::Printf(TEXT("%s %s %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *Name, *Detail);
	UE_LOG(LogTemp, Display, TEXT("CLTEST %s"), *Line);
	FFileHelper::SaveStringToFile(Line + TEXT("\n"), *FPaths::Combine(CaptureDir, TEXT("results.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

void ACLPlayerController::Perf(const FString& Name)
{
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	if (!M) return;
	// mean over the last half second: single timer readings jitter by tens of percent
	FCLGpuTimes G = M->Gpu;
	if (GpuHist.Num() > 0)
	{
		G.LightMs = G.NearMs = G.FarMs = G.CompositeMs = 0.f;
		for (const FCLGpuTimes& H : GpuHist) { G.LightMs += H.LightMs; G.NearMs += H.NearMs; G.FarMs += H.FarMs; G.CompositeMs += H.CompositeMs; }
		const float Inv = 1.f / GpuHist.Num();
		G.LightMs *= Inv; G.NearMs *= Inv; G.FarMs *= Inv; G.CompositeMs *= Inv;
	}
	const FString Line = FString::Printf(TEXT("PERF %s light %.3f near %.3f far %.3f composite %.3f total %.3f ms (near %dx%d far %dx%d, %d light slices) frame %.1f ms"),
		*Name, G.LightMs, G.NearMs, G.FarMs, G.CompositeMs, G.LightMs + G.NearMs + G.FarMs + G.CompositeMs, G.NearSize.X, G.NearSize.Y, G.FarSize.X, G.FarSize.Y, G.LightSlices, FrameMs);
	UE_LOG(LogTemp, Display, TEXT("CLTEST %s"), *Line);
	FFileHelper::SaveStringToFile(Line + TEXT("\n"), *FPaths::Combine(CaptureDir, TEXT("results.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

void ACLPlayerController::Hover(const FVector& DomainM, const FVector& Dir)
{
	if (ACLPlane* P = GetPlane())
	{
		P->ResetTo(ACLWorld::DomainToWorld(DomainM), Dir.GetSafeNormal(), 60.f);
		P->bFrozen = true;
		AimYaw = P->Aim.Yaw; AimPitch = P->Aim.Pitch;
	}
}

void ACLPlayerController::TickCapture(float Dt)
{
	static TArray<FStep> Script;
	static bool bBuilt = false;
	static float T0 = -1.f;
	ACLRace* Race = ACLRace::Get(this);
	ACLCloudManager* Mg = ACLCloudManager::Get(this);
	if (!Race || !Mg) return;
	if (T0 < 0.f)
	{
		if (!Mg->IsReady() || Race->State == ECLRaceState::Loading) return;
		T0 = GetWorld()->GetTimeSeconds();
		IFileManager::Get().Delete(*FPaths::Combine(CaptureDir, TEXT("results.txt")));
		IFileManager::Get().Delete(*FPaths::Combine(CaptureDir, TEXT("motion.csv")));
	}
	CaptureT = GetWorld()->GetTimeSeconds() - T0;
	GpuHist.Add(Mg->Gpu);
	if (GpuHist.Num() > 30) GpuHist.RemoveAt(0);
	if (!bBuilt)
	{
		bBuilt = true;
		auto Field = [](ACLPlayerController* P) { return ACLCloudManager::Get(P)->GetField(); };
		auto R = [](ACLPlayerController* P) -> FCLRenderSettings& { return ACLCloudManager::Get(P)->Render; };
		auto Ring = [Field](ACLPlayerController* P, int32 I) { return Field(P)->Course[I]; };
		// a viewpoint a little before ring I, looking along the course
		auto Before = [Ring](ACLPlayerController* P, int32 I, float Back, float Up = 0.f)
		{
			const FCLRingSpot S = Ring(P, I);
			P->Hover(S.PosM - S.Dir * Back + FVector(0, 0, Up), S.Dir);
		};
		float t = 0.5f;
		auto At = [&t](float Dt) { t += Dt; return t; };
		Script = {
			{ At(0), [Field](ACLPlayerController* P)
			{
				ACLRace::Get(P)->bCaptureHold = true;
				const FCLCloudField* F = Field(P);
				const FCLCloudField::FStats& S = F->Stats;
				P->Check(S.Occupancy > 0.02f && S.EnvelopeOccupancy > S.Occupancy, TEXT("cloudscape"),
					FString::Printf(TEXT("built in %.1f s (shape %.0f ms, EDT %.0f, envelope %.0f, encode %.0f, noise %.0f); occupancy voxel %.3f envelope %.3f"),
						ACLCloudManager::Get(P)->BuildSeconds, S.ShapeMs, S.EdtMs, S.EnvelopeMs, S.EncodeMs, S.NoiseMs, S.Occupancy, S.EnvelopeOccupancy));
				P->Check(S.BC1SafeOver == 0 && S.BC4Over == 0 && S.BC1NaiveOver > 0, TEXT("sdf_conservative"),
					FString::Printf(TEXT("texels above the true distance: BC1 safe %lld, BC4 log %lld, BC1 nearest %lld; mean under-estimate BC1 %.2f m BC4 %.2f m"),
						S.BC1SafeOver, S.BC4Over, S.BC1NaiveOver, S.BC1SafeMeanUnder, S.BC4MeanUnder));
				// tunnel centre: free air in the voxel cloudscape, filled by the 2.5D envelope
				const FVector C = F->Course[2].PosM;
				const float Dv = F->Density(C, ECLRepresentation::Voxel), De = F->Density(C, ECLRepresentation::Envelope), Sv = F->Sdf(C, ECLRepresentation::Voxel);
				P->Check(Dv < 0.01f && De > 0.05f && Sv > 5.f, TEXT("tunnel_representation"), FString::Printf(TEXT("tunnel centre: voxel density %.3f (%.0f m to the cloud wall), envelope density %.3f"), Dv, Sv, De));
				int32 Clear = 0;
				for (const FCLRingSpot& Sp : F->Course) if (F->Sdf(Sp.PosM, ECLRepresentation::Voxel) > Sp.RadiusM * 0.6f) ++Clear;
				P->Check(Clear == F->Course.Num(), TEXT("rings_in_clear_air"), FString::Printf(TEXT("%d of %d rings have clear air around them"), Clear, F->Course.Num()));
			} },
			// overview from the start
			{ At(0.1f), [](ACLPlayerController* P) { const FCLCloudField* F = ACLCloudManager::Get(P)->GetField(); P->Hover(F->StartM, F->StartDir); } },
			{ At(2.5f), [](ACLPlayerController* P) { P->Shot(TEXT("01_start")); P->Perf(TEXT("start_default")); } },
			// the tunnel: voxel vs envelope
			{ At(0.5f), [Before](ACLPlayerController* P) { Before(P, 1, 120.f); } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("02_tunnel_voxel")); P->Perf(TEXT("tunnel_voxel")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Rep = ECLRepresentation::Envelope; } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("03_tunnel_envelope")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Rep = ECLRepresentation::Voxel; } },
			// inside the tunnel
			{ At(0.3f), [Before](ACLPlayerController* P) { Before(P, 2, 70.f); } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("04_in_tunnel")); P->Perf(TEXT("in_tunnel_split")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 3; } },
			{ At(1.0f), [](ACLPlayerController* P) { P->Shot(TEXT("04b_tunnel_sunT")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 0; R(P).bMultiScatter = false; } },
			{ At(1.0f), [](ACLPlayerController* P) { P->Shot(TEXT("04c_tunnel_noMS")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).bMultiScatter = true; R(P).bAmbient = false; } },
			{ At(1.0f), [](ACLPlayerController* P) { P->Shot(TEXT("04d_tunnel_noAmb")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).bAmbient = true; } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Resolution = 1; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Perf(TEXT("in_tunnel_half")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Resolution = 0; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Perf(TEXT("in_tunnel_full")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Resolution = 2; R(P).DebugView = 4; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("05_split_view")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 0; } },
			// between clouds: SDF skipping and the step budget
			{ At(0.3f), [Before](ACLPlayerController* P) { Before(P, 4, 260.f, 10.f); } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("06_between")); P->Perf(TEXT("between_sdf")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 1; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("07_iters_sdf")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).bSdfSkip = false; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("08_iters_nosdf")); P->Perf(TEXT("between_nosdf")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 0; R(P).MaxSteps = 256; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("09_cap256_nosdf")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).bSdfSkip = true; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("10_cap256_sdf")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).MaxSteps = 1024; R(P).SdfFormat = ECLSdfFormat::BC1Naive; R(P).DebugView = 5; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("11_overshoot_naive")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).SdfFormat = ECLSdfFormat::BC1Safe; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("12_overshoot_safe")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).DebugView = 0; R(P).Lighting = 0; } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Perf(TEXT("between_lightmarch")); P->Shot(TEXT("13_lightmarch")); } },
			{ At(0.2f), [R](ACLPlayerController* P) { R(P).Lighting = 1; } },
			// cave and storm
			{ At(0.3f), [Before](ACLPlayerController* P) { Before(P, 6, 60.f); } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("14_cave")); P->Perf(TEXT("cave")); } },
			{ At(0.3f), [Before](ACLPlayerController* P) { Before(P, 10, 70.f); } },
			{ At(1.0f), [Field](ACLPlayerController* P) { ACLCloudManager::Get(P)->ForceStrike(ACLWorld::DomainToWorld(Field(P)->StormCoreM + FVector(40, 30, 40))); } },
			{ At(0.06f), [](ACLPlayerController* P) { P->Shot(TEXT("15_storm_flash")); } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("16_storm_dark")); } },
			// outside view of the storm cloud and the evening sun
			{ At(0.3f), [Field](ACLPlayerController* P) { const FVector C = Field(P)->StormCoreM; P->Hover(C + FVector(-1100, -900, 60), FVector(1100, 900, -20)); } },
			{ At(2.0f), [](ACLPlayerController* P) { P->Shot(TEXT("17_storm_outside")); } },
			{ At(0.2f), [](ACLPlayerController* P) { ACLWorld::Get(P)->SetTimeOfDay(ECLTimeOfDay::Evening); } },
			{ At(2.5f), [](ACLPlayerController* P) { P->Shot(TEXT("18_evening")); P->Perf(TEXT("evening_rebuild_settled")); } },
			{ At(0.2f), [](ACLPlayerController* P) { ACLWorld::Get(P)->SetTimeOfDay(ECLTimeOfDay::Morning); } },
			// principle panel open (layout check)
			{ At(0.3f), [](ACLPlayerController* P) { const FCLCloudField* F = ACLCloudManager::Get(P)->GetField(); P->Hover(F->StartM, F->StartDir); P->TogglePanel(); } },
			{ At(1.5f), [](ACLPlayerController* P) { P->Shot(TEXT("23_panel")); } },
			{ At(0.2f), [](ACLPlayerController* P) { P->TogglePanel(); } },
			// UE's own volumetric cloud fed with the same data
			{ At(0.3f), [](ACLPlayerController* P) { const FCLCloudField* F = ACLCloudManager::Get(P)->GetField(); P->Hover(F->StartM, F->StartDir); ACLCloudManager::Get(P)->SetRoute(ECLRoute::UECloud); } },
			{ At(3.0f), [](ACLPlayerController* P) { P->Shot(TEXT("19_uecloud")); } },
			{ At(0.2f), [](ACLPlayerController* P) { ACLCloudManager::Get(P)->SetRoute(ECLRoute::Custom); } },
			// icing: fly straight into the body of the tunnel cloud (beside the tunnel)
			{ At(0.5f), [Ring](ACLPlayerController* P) { const FCLRingSpot S = Ring(P, 2); P->Hover(S.PosM + FVector(0, 0, 90) - S.Dir * 40.f, S.Dir); ACLPlane* Pl = P->GetPlane(); Pl->bFrozen = false; } },
			{ At(3.0f), [](ACLPlayerController* P) { ACLPlane* Pl = P->GetPlane(); P->Check(Pl->Ice > 0.3f, TEXT("icing"), FString::Printf(TEXT("ice %.2f after 3 s in cloud (density at plane %.2f)"), Pl->Ice, Pl->CloudDensity)); P->Shot(TEXT("20_icing")); } },
			// GPU cost of each mechanism at three viewpoints (custom renderer, debug views off)
			{ At(0.3f), [](ACLPlayerController* P) { P->Metrics.Add(TEXT("perf_t"), P->CaptureT); } },
			// the course on autopilot
			{ At(1.4f * 31 + 0.5f), [](ACLPlayerController* P) { ACLRace* Rc = ACLRace::Get(P); Rc->bCaptureHold = false; Rc->bAutoPilot = true; Rc->Restart(); P->Metrics.Add(TEXT("auto_t"), P->CaptureT); } },
			{ At(30.f), [](ACLPlayerController* P) { P->Shot(TEXT("21_autopilot_30s")); P->Perf(TEXT("autopilot_30s")); } },
			{ At(60.f), [](ACLPlayerController* P)
			{
				ACLRace* Rc = ACLRace::Get(P);
				P->Check(Rc->State == ECLRaceState::Finished || Rc->Next >= 10, TEXT("course_flown"), FString::Printf(TEXT("autopilot passed %d of %d rings, state %d, time %.1f s, crashes %d"), Rc->Next, Rc->Rings.Num(), (int32)Rc->State, Rc->RaceSeconds(), Rc->Crashes));
				P->Shot(TEXT("22_autopilot_end"));
			} },
			{ At(0.5f), [](ACLPlayerController* P) { UKismetSystemLibrary::QuitGame(P, P, EQuitPreference::Quit, false); } },
		};
	}
	// motion record while the autopilot flies through the tunnel cloud: per-frame plane screen position and camera
	// path (to measure judder), plus a short run of frame dumps for visual review
	if (const float* AutoT = Metrics.Find(TEXT("auto_t")))
	{
		const float Rel = CaptureT - *AutoT;
		static int32 Frame = 0;
		if (Rel > 8.f && Rel < 12.f)
		{
			ACLPlane* Pl = GetPlane();
			FVector2D Sp;
			ProjectWorldLocationToScreen(Pl->GetActorLocation(), Sp);
			const FVector C = PlayerCameraManager->GetCameraLocation();
			const FString Line = FString::Printf(TEXT("%d,%.5f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n"), Frame, Dt, Sp.X, Sp.Y, C.X, C.Y, C.Z, Pl->GetActorLocation().X, Pl->GetActorLocation().Y, Pl->GetActorLocation().Z);
			FFileHelper::SaveStringToFile(Line, *FPaths::Combine(CaptureDir, TEXT("motion.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
			if (Frame < 48 && FParse::Param(FCommandLine::Get(), TEXT("CLFrames"))) Shot(FString::Printf(TEXT("frames/f_%03d"), Frame));
			++Frame;
		}
	}
	if (const float* PerfT = Metrics.Find(TEXT("perf_t")))
	{
		struct FCfg { const TCHAR* Name; int32 Step; bool bSdf; int32 Light; int32 Res; bool bMip; ECLSdfFormat Fmt; };
		static const FCfg Cfgs[] = {
			{ TEXT("base_2p5d_step_lightmarch_half"), 0, false, 0, 1, true, ECLSdfFormat::BC1Safe },
			{ TEXT("plus_lightgrid"), 0, false, 1, 1, true, ECLSdfFormat::BC1Safe },
			{ TEXT("adaptive_step"), 1, false, 1, 1, true, ECLSdfFormat::BC1Safe },
			{ TEXT("plus_sdf_bc1"), 1, true, 1, 1, true, ECLSdfFormat::BC1Safe },
			{ TEXT("plus_split"), 1, true, 1, 2, true, ECLSdfFormat::BC1Safe },
			{ TEXT("split_sdf_r16"), 1, true, 1, 2, true, ECLSdfFormat::R16 },
			{ TEXT("split_sdf_bc4log"), 1, true, 1, 2, true, ECLSdfFormat::BC4Log },
			{ TEXT("split_noise_mip0"), 1, true, 1, 2, false, ECLSdfFormat::BC1Safe },
			{ TEXT("split_lightmarch"), 1, true, 0, 2, true, ECLSdfFormat::BC1Safe },
			{ TEXT("full_res"), 1, true, 1, 0, true, ECLSdfFormat::BC1Safe },
		};
		const int32 NC = UE_ARRAY_COUNT(Cfgs), NV = 3;
		const float Slot = 1.4f;
		const int32 K = FMath::FloorToInt((CaptureT - *PerfT) / Slot);
		static int32 Done = -1;
		if (K > Done && K <= NC * NV)
		{
			ACLCloudManager* M = ACLCloudManager::Get(this);
			const FCLCloudField* F = M->GetField();
			if (Done >= 0)	// measure the configuration set one slot ago
			{
				const FCfg& C = Cfgs[Done % NC];
				static const TCHAR* Views[] = { TEXT("far"), TEXT("between"), TEXT("inside") };
				Perf(FString::Printf(TEXT("%s/%s"), Views[Done / NC], C.Name));
			}
			Done = K;
			if (K < NC * NV)
			{
				const int32 V = K / NC;
				if (K % NC == 0)
				{
					if (V == 0) Hover(F->StartM, F->StartDir);
					if (V == 1) { const FCLRingSpot& S = F->Course[4]; Hover(S.PosM - S.Dir * 260.f + FVector(0, 0, 10), S.Dir); }
					if (V == 2) { const FCLRingSpot& S = F->Course[2]; Hover(S.PosM - S.Dir * 70.f, S.Dir); }
				}
				const FCfg& C = Cfgs[K % NC];
				FCLRenderSettings& R = M->Render;
				R.DebugView = 0; R.StepMode = C.Step; R.bSdfSkip = C.bSdf; R.Lighting = C.Light; R.Resolution = C.Res; R.bNoiseMipByDistance = C.bMip; R.SdfFormat = C.Fmt; R.MaxSteps = 4096;
			}
			else
			{
				FCLRenderSettings& R = M->Render;
				R = FCLRenderSettings();
				Metrics.Remove(TEXT("perf_t"));
			}
		}
	}
	for (int32 I = 0; I < Script.Num(); ++I)
		if (Script[I].Do && CaptureT >= Script[I].At) { auto F = MoveTemp(Script[I].Do); Script[I].Do = nullptr; F(this); }
}
