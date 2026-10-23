#include "CLPlayerController.h"

#include "CLCloudManager.h"
#include "CLHud.h"
#include "CLPlane.h"
#include "CLRace.h"
#include "CLWorld.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ACLPlayerController::ACLPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ACLPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController()) return;
	FParse::Value(FCommandLine::Get(), TEXT("-CLCapture="), CaptureDir);
	Hud = CreateWidget<UCLHud>(this, UCLHud::StaticClass());
	if (Hud) Hud->AddToViewport();
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
	FTimerHandle H;
	GetWorldTimerManager().SetTimer(H, [this]()
	{
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWindow().IsValid())
			GEngine->GameViewport->GetWindow()->SetTitle(FText::FromString(TEXT("穿云竞速 · 可穿越的体素体积云")));
	}, 0.5f, false);
}

ACLPlane* ACLPlayerController::GetPlane() const { return Cast<ACLPlane>(GetPawn()); }

void ACLPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &ACLPlayerController::TogglePanel);
	InputComponent->BindKey(EKeys::F1, IE_Pressed, this, &ACLPlayerController::OnHelp);
	InputComponent->BindKey(EKeys::V, IE_Pressed, this, &ACLPlayerController::OnCycleView);
	InputComponent->BindKey(EKeys::C, IE_Pressed, this, &ACLPlayerController::OnCam);
	InputComponent->BindKey(EKeys::R, IE_Pressed, this, &ACLPlayerController::OnRespawn);
	InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &ACLPlayerController::OnRestart);
	InputComponent->BindAxisKey(EKeys::MouseX, this, &ACLPlayerController::OnTurn);
	InputComponent->BindAxisKey(EKeys::MouseY, this, &ACLPlayerController::OnLookUp);
}

void ACLPlayerController::OnTurn(float V) { if (!bPanel) AimYaw += V * 0.45f; }
void ACLPlayerController::OnLookUp(float V) { if (!bPanel) AimPitch = FMath::Clamp(AimPitch - V * 0.45f, -80.f, 80.f); }

void ACLPlayerController::OnCycleView()
{
	ACLCloudManager* M = ACLCloudManager::Get(this);
	if (M) M->Render.DebugView = (M->Render.DebugView + 1) % 6;
}

void ACLPlayerController::TogglePanel()
{
	bPanel = !bPanel;
	bShowMouseCursor = bPanel;
	if (bPanel)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
		SetInputMode(Mode);
	}
	else SetInputMode(FInputModeGameOnly());
}

void ACLPlayerController::PlayerTick(float Dt)
{
	Super::PlayerTick(Dt);
	FrameMs = FMath::Lerp(FrameMs, Dt * 1000.f, 0.05f);
	ACLPlane* P = GetPlane();
	if (P)
	{
		if (!bAimInit || P->bFrozen) { AimYaw = P->Aim.Yaw; AimPitch = P->Aim.Pitch; bAimInit = true; }
		ACLRace* R = ACLRace::Get(this);
		const bool bAuto = R && R->bAutoPilot;
		if (!bAuto && !P->bFrozen) P->Aim = FRotator(AimPitch, AimYaw, 0);
		if (bAuto) { AimYaw = P->Aim.Yaw; AimPitch = P->Aim.Pitch; }
		P->ThrottleInput = bPanel ? 0.f : (IsInputKeyDown(EKeys::W) ? 1.f : 0.f) - (IsInputKeyDown(EKeys::S) ? 1.f : 0.f);
		P->RollInput = bPanel ? 0.f : (IsInputKeyDown(EKeys::D) ? 1.f : 0.f) - (IsInputKeyDown(EKeys::A) ? 1.f : 0.f);
	}
	if (!CaptureDir.IsEmpty()) TickCapture(Dt);
}

// ---------------------------------------------------------------- principle panel

namespace
{
	struct FLabInfo { const TCHAR* Name; const TCHAR* Hint; const TCHAR* Key; };
	const FLabInfo& Info(ECLLab W)
	{
		static const FLabInfo Table[(int32)ECLLab::Count] = {
			{ TEXT("自研体素云渲染"), TEXT("本工程的 RDG 光线步进：下面所有开关都作用于它"), TEXT("") },
			{ TEXT("UE 自带体积云组件"), TEXT("同一份体素数据交给 UE 5.8 的 Volumetric Cloud 组件绘制：没有 SDF 跳空，飞进云里时看它的时间重建"), TEXT("") },
			{ TEXT("三维体素"), TEXT("512×512×64、每格 8 m 的稠密网格：隧道、云洞、悬垂都能表示"), TEXT("") },
			{ TEXT("2.5D 包络"), TEXT("同一片云景按列只保留最低点和最高点（讲座的包络法）：隧道和云洞被填实，飞进去会结冰"), TEXT("") },
			{ TEXT("噪声细节提升"), TEXT("用 128³ 噪声侵蚀 8 m 的维度轮廓；关掉则只画轮廓本身"), TEXT("") },
			{ TEXT("近处二次折叠噪声"), TEXT("150 m 内把噪声两次取绝对值折叠，得到更高一级的频率"), TEXT("") },
			{ TEXT("噪声 MIP 随距离"), TEXT("远处采样较粗的 MIP：讲座称约快 15%"), TEXT("") },
			{ TEXT("2.5D 距离步长"), TEXT("步长 3 + 60·d/16384 m，只随距离增大"), TEXT("") },
			{ TEXT("自适应步长"), TEXT("max(1, 0.08·√d) m：云内采样更密"), TEXT("") },
			{ TEXT("SDF 跳空"), TEXT("云外按到最近云的距离跳步，只在 SDF < 0 处采样"), TEXT("") },
			{ TEXT("R16（未压缩）"), TEXT("每体素 2 字节，误差约 0.07 m"), TEXT("") },
			{ TEXT("BC1 565 拆分 · 保守"), TEXT("讲座的编码，按 NVIDIA/AMD/Intel 解码结果的上界编码：永不越界"), TEXT("") },
			{ TEXT("BC1 565 拆分 · 不保守"), TEXT("按最近值编码：解码后可能大于真实距离，用“SDF 越界”视图查看"), TEXT("") },
			{ TEXT("BC4 对数映射 · 保守"), TEXT("单通道 8 个调色板值，表面附近更准"), TEXT("") },
			{ TEXT("128 步"), TEXT("每条光线最多 128 次循环"), TEXT("") },
			{ TEXT("256 步"), TEXT("每条光线最多 256 次循环"), TEXT("") },
			{ TEXT("512 步"), TEXT("每条光线最多 512 次循环"), TEXT("") },
			{ TEXT("不限"), TEXT("最多 4096 次循环"), TEXT("") },
			{ TEXT("10 次光照步进"), TEXT("每个云内采样再朝太阳走 10 步（256 m）：贵，且没有远距离云影"), TEXT("") },
			{ TEXT("光照网格"), TEXT("近处 2 次直接采样，其余查 256×256×32 的预计算累积密度，8 帧更新一遍"), TEXT("") },
			{ TEXT("多重散射近似"), TEXT("把维度轮廓当作多次散射的概率场：逆光时云内发亮，顺光时边缘发暗"), TEXT("") },
			{ TEXT("环境光"), TEXT("天空光按 (1 − 轮廓)^0.5 渗入云的表层"), TEXT("") },
			{ TEXT("环境光方向性"), TEXT("用光照网格里朝天空的累积密度遮挡环境光：上方有云处更暗"), TEXT("") },
			{ TEXT("闪电"), TEXT("雷暴云里的闪电按球形衰减的概率场照亮云内"), TEXT("") },
			{ TEXT("全分辨率"), TEXT("每个像素都步进：最贵"), TEXT("") },
			{ TEXT("半分辨率"), TEXT("步进像素减为 1/4（讲座 30 Hz 模式的 960×540）"), TEXT("") },
			{ TEXT("按 200 m 拆分两档"), TEXT("200 m 内 1/4 分辨率、之外 1/2 分辨率（讲座 60 Hz 模式）"), TEXT("") },
			{ TEXT("正常画面"), TEXT(""), TEXT("V") },
			{ TEXT("循环次数热图"), TEXT("每个像素的步进循环次数：开关 SDF 跳空对比"), TEXT("") },
			{ TEXT("云内采样数热图"), TEXT("真正做了密度和光照计算的采样数"), TEXT("") },
			{ TEXT("只看太阳透射"), TEXT("只显示每个采样点收到的太阳光，看远距离云影"), TEXT("") },
			{ TEXT("两档分界"), TEXT("200 m 内的近层染成橙色，远层染成蓝色"), TEXT("") },
			{ TEXT("SDF 越界"), TEXT("解码后的距离大于真实距离的采样点标红"), TEXT("") },
			{ TEXT("清晨"), TEXT("低太阳：长云影、暖色"), TEXT("") },
			{ TEXT("正午"), TEXT("高太阳"), TEXT("") },
			{ TEXT("傍晚"), TEXT("逆光看云：内部辉光最明显"), TEXT("") },
			{ TEXT("重新开始"), TEXT("回到起点重新计时"), TEXT("Enter") },
			{ TEXT("回到上一个检查环"), TEXT("坠毁后从上一个检查环继续，罚时 5 秒"), TEXT("R") },
			{ TEXT("座舱视角"), TEXT("在追尾视角和座舱视角之间切换"), TEXT("C") },
		};
		return Table[(int32)W];
	}
}

FText ACLPlayerController::LabName(ECLLab W) { return FText::FromString(Info(W).Name); }
FText ACLPlayerController::LabHint(ECLLab W) { return FText::FromString(Info(W).Hint); }
FString ACLPlayerController::LabKey(ECLLab W) { return Info(W).Key; }

bool ACLPlayerController::LabOn(ECLLab W) const
{
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	if (!M) return false;
	const FCLRenderSettings& R = M->Render;
	const ACLWorld* Wd = ACLWorld::Get(this);
	const ACLPlane* P = GetPlane();
	switch (W)
	{
	case ECLLab::RouteCustom: return M->Route == ECLRoute::Custom;
	case ECLLab::RouteUE: return M->Route == ECLRoute::UECloud;
	case ECLLab::RepVoxel: return R.Rep == ECLRepresentation::Voxel;
	case ECLLab::RepEnvelope: return R.Rep == ECLRepresentation::Envelope;
	case ECLLab::Uprez: return R.Detail >= 1;
	case ECLLab::NearFold: return R.Detail >= 2;
	case ECLLab::NoiseMip: return R.bNoiseMipByDistance;
	case ECLLab::StepLegacy: return R.StepMode == 0;
	case ECLLab::StepAdaptive: return R.StepMode == 1;
	case ECLLab::SdfSkip: return R.bSdfSkip;
	case ECLLab::SdfR16: return R.SdfFormat == ECLSdfFormat::R16;
	case ECLLab::SdfBC1: return R.SdfFormat == ECLSdfFormat::BC1Safe;
	case ECLLab::SdfBC1Naive: return R.SdfFormat == ECLSdfFormat::BC1Naive;
	case ECLLab::SdfBC4: return R.SdfFormat == ECLSdfFormat::BC4Log;
	case ECLLab::Cap128: return R.MaxSteps == 128;
	case ECLLab::Cap256: return R.MaxSteps == 256;
	case ECLLab::Cap512: return R.MaxSteps == 512;
	case ECLLab::CapNone: return R.MaxSteps >= 4096;
	case ECLLab::LightMarch: return R.Lighting == 0;
	case ECLLab::LightGrid: return R.Lighting == 1;
	case ECLLab::MultiScatter: return R.bMultiScatter;
	case ECLLab::Ambient: return R.bAmbient;
	case ECLLab::AmbientDir: return R.bAmbientDirectional;
	case ECLLab::Lightning: return R.bLightning;
	case ECLLab::ResFull: return R.Resolution == 0;
	case ECLLab::ResHalf: return R.Resolution == 1;
	case ECLLab::ResSplit: return R.Resolution == 2;
	case ECLLab::ViewNormal: return R.DebugView == 0;
	case ECLLab::ViewIters: return R.DebugView == 1;
	case ECLLab::ViewSamples: return R.DebugView == 2;
	case ECLLab::ViewLight: return R.DebugView == 3;
	case ECLLab::ViewSplit: return R.DebugView == 4;
	case ECLLab::ViewOver: return R.DebugView == 5;
	case ECLLab::TodMorning: return Wd && Wd->GetTimeOfDay() == ECLTimeOfDay::Morning;
	case ECLLab::TodNoon: return Wd && Wd->GetTimeOfDay() == ECLTimeOfDay::Noon;
	case ECLLab::TodEvening: return Wd && Wd->GetTimeOfDay() == ECLTimeOfDay::Evening;
	case ECLLab::CockpitCam: return P && P->bCockpitCam;
	default: return false;
	}
}

bool ACLPlayerController::LabEnabled(ECLLab W) const
{
	const ACLCloudManager* M = ACLCloudManager::Get(this);
	if (!M) return false;
	const FCLRenderSettings& R = M->Render;
	const bool bCustom = M->Route == ECLRoute::Custom;
	switch (W)
	{
	case ECLLab::RouteCustom: case ECLLab::RouteUE: return true;
	case ECLLab::RepVoxel: case ECLLab::RepEnvelope: return bCustom;
	case ECLLab::Uprez: return bCustom;
	case ECLLab::NearFold: case ECLLab::NoiseMip: return bCustom && R.Detail >= 1;
	case ECLLab::StepLegacy: case ECLLab::StepAdaptive: case ECLLab::SdfSkip:
	case ECLLab::Cap128: case ECLLab::Cap256: case ECLLab::Cap512: case ECLLab::CapNone: return bCustom;
	case ECLLab::SdfR16: case ECLLab::SdfBC1: case ECLLab::SdfBC1Naive: case ECLLab::SdfBC4: return bCustom && R.bSdfSkip;
	case ECLLab::LightMarch: case ECLLab::LightGrid: case ECLLab::MultiScatter: case ECLLab::Ambient: case ECLLab::Lightning: return bCustom;
	case ECLLab::AmbientDir: return bCustom && R.Lighting == 1 && R.bAmbient;
	case ECLLab::ResFull: case ECLLab::ResHalf: case ECLLab::ResSplit: return bCustom;
	case ECLLab::ViewNormal: case ECLLab::ViewIters: case ECLLab::ViewSamples: case ECLLab::ViewLight: case ECLLab::ViewSplit: return bCustom;
	case ECLLab::ViewOver: return bCustom && R.bSdfSkip && R.SdfFormat != ECLSdfFormat::R16;
	default: return true;
	}
}

void ACLPlayerController::Lab(ECLLab W)
{
	ACLCloudManager* M = ACLCloudManager::Get(this);
	ACLWorld* Wd = ACLWorld::Get(this);
	ACLRace* Rc = ACLRace::Get(this);
	ACLPlane* P = GetPlane();
	if (!M) return;
	if (!LabEnabled(W)) return;
	FCLRenderSettings& R = M->Render;
	switch (W)
	{
	case ECLLab::RouteCustom: M->SetRoute(ECLRoute::Custom); break;
	case ECLLab::RouteUE: M->SetRoute(ECLRoute::UECloud); break;
	case ECLLab::RepVoxel: R.Rep = ECLRepresentation::Voxel; break;
	case ECLLab::RepEnvelope: R.Rep = ECLRepresentation::Envelope; break;
	case ECLLab::Uprez: R.Detail = R.Detail >= 1 ? 0 : 2; break;
	case ECLLab::NearFold: R.Detail = R.Detail >= 2 ? 1 : 2; break;
	case ECLLab::NoiseMip: R.bNoiseMipByDistance = !R.bNoiseMipByDistance; break;
	case ECLLab::StepLegacy: R.StepMode = 0; break;
	case ECLLab::StepAdaptive: R.StepMode = 1; break;
	case ECLLab::SdfSkip: R.bSdfSkip = !R.bSdfSkip; if (!R.bSdfSkip && R.DebugView == 5) R.DebugView = 0; break;
	case ECLLab::SdfR16: R.SdfFormat = ECLSdfFormat::R16; if (R.DebugView == 5) R.DebugView = 0; break;
	case ECLLab::SdfBC1: R.SdfFormat = ECLSdfFormat::BC1Safe; break;
	case ECLLab::SdfBC1Naive: R.SdfFormat = ECLSdfFormat::BC1Naive; break;
	case ECLLab::SdfBC4: R.SdfFormat = ECLSdfFormat::BC4Log; break;
	case ECLLab::Cap128: R.MaxSteps = 128; break;
	case ECLLab::Cap256: R.MaxSteps = 256; break;
	case ECLLab::Cap512: R.MaxSteps = 512; break;
	case ECLLab::CapNone: R.MaxSteps = 4096; break;
	case ECLLab::LightMarch: R.Lighting = 0; break;
	case ECLLab::LightGrid: R.Lighting = 1; break;
	case ECLLab::MultiScatter: R.bMultiScatter = !R.bMultiScatter; break;
	case ECLLab::Ambient: R.bAmbient = !R.bAmbient; break;
	case ECLLab::AmbientDir: R.bAmbientDirectional = !R.bAmbientDirectional; break;
	case ECLLab::Lightning: R.bLightning = !R.bLightning; break;
	case ECLLab::ResFull: R.Resolution = 0; break;
	case ECLLab::ResHalf: R.Resolution = 1; break;
	case ECLLab::ResSplit: R.Resolution = 2; break;
	case ECLLab::ViewNormal: R.DebugView = 0; break;
	case ECLLab::ViewIters: R.DebugView = 1; break;
	case ECLLab::ViewSamples: R.DebugView = 2; break;
	case ECLLab::ViewLight: R.DebugView = 3; break;
	case ECLLab::ViewSplit: R.DebugView = 4; break;
	case ECLLab::ViewOver: R.DebugView = 5; break;
	case ECLLab::TodMorning: if (Wd) Wd->SetTimeOfDay(ECLTimeOfDay::Morning); break;
	case ECLLab::TodNoon: if (Wd) Wd->SetTimeOfDay(ECLTimeOfDay::Noon); break;
	case ECLLab::TodEvening: if (Wd) Wd->SetTimeOfDay(ECLTimeOfDay::Evening); break;
	case ECLLab::Restart: if (Rc) Rc->Restart(); break;
	case ECLLab::Respawn: if (Rc) Rc->RespawnAtLastRing(); break;
	case ECLLab::CockpitCam: if (P) P->bCockpitCam = !P->bCockpitCam; break;
	default: break;
	}
}
