#include "CLHud.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "CLCloudField.h"
#include "CLCloudManager.h"
#include "CLPlane.h"
#include "CLRace.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"

namespace
{
	// deep-slate panels, sky-blue accent, amber for the next ring and warnings
	const FLinearColor Fill(0.012f, 0.02f, 0.032f, 0.80f);
	const FLinearColor FillSoft(0.015f, 0.024f, 0.036f, 0.62f);
	const FLinearColor Rim(0.45f, 0.58f, 0.70f, 0.55f);
	const FLinearColor Ink(0.95f, 0.96f, 0.97f);
	const FLinearColor Dim(0.64f, 0.70f, 0.76f);
	const FLinearColor Sky(0.45f, 0.78f, 1.0f);
	const FLinearColor Amber(1.0f, 0.70f, 0.25f);
	const FLinearColor Good(0.45f, 0.92f, 0.55f);
	const FLinearColor Bad(1.0f, 0.40f, 0.32f);
	const FLinearColor Ice(0.62f, 0.90f, 1.0f);
	const FLinearColor Off(0.30f, 0.34f, 0.38f, 1.f);
	const FLinearColor CapFill(0.90f, 0.92f, 0.93f);
	const FLinearColor CapInk(0.04f, 0.05f, 0.07f);

	FSlateBrush Rounded(FLinearColor F, float R, FLinearColor O = FLinearColor::Transparent, float W = 0.f)
	{
		FSlateRoundedBoxBrush B(F, R);
		B.OutlineSettings = FSlateBrushOutlineSettings(FVector4(R, R, R, R), FSlateColor(O), W);
		return B;
	}
	FText T(const FString& S) { return FText::FromString(S); }
	FString Mmss(float S) { S = FMath::Max(S, 0.f); const int32 M = int32(S) / 60; return FString::Printf(TEXT("%d:%05.2f"), M, S - M * 60); }
}

void UCLClick::OnClicked() { if (ACLPlayerController* P = Owner.Get()) P->Lab(Which); }

ACLPlayerController* UCLHud::PC() const { return Cast<ACLPlayerController>(GetOwningPlayer()); }

UTextBlock* UCLHud::Text(int32 Size, FLinearColor Color, bool bBold, float Wrap)
{
	UTextBlock* B = WidgetTree->ConstructWidget<UTextBlock>();
	B->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
	B->SetColorAndOpacity(FSlateColor(Color));
	B->SetShadowOffset(FVector2D(1.f, 1.f));
	B->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));
	if (Wrap > 0.f) { B->SetAutoWrapText(false); B->SetWrapTextAt(Wrap); }
	return B;
}

UBorder* UCLHud::Panel(FLinearColor F, FLinearColor R, float Radius, FMargin Pad)
{
	UBorder* B = WidgetTree->ConstructWidget<UBorder>();
	B->SetBrush(Rounded(F, Radius, R, R.A > 0.f ? 1.2f : 0.f));
	B->SetPadding(Pad);
	return B;
}

UBorder* UCLHud::KeyCap(const FString& Key, int32 Size)
{
	UBorder* Cap = Panel(CapFill, FLinearColor(0.2f, 0.22f, 0.25f, 1.f), 4.f, FMargin(7, 1, 7, 2));
	UTextBlock* Tx = Text(Size, CapInk, true);
	Tx->SetShadowColorAndOpacity(FLinearColor::Transparent);
	Tx->SetText(T(Key));
	Cap->SetContent(Tx);
	Cap->SetHorizontalAlignment(HAlign_Center);
	return Cap;
}

UCanvasPanelSlot* UCLHud::Put(UWidget* W, FVector2D Anchor, FVector2D Align, FVector2D Pos)
{
	UCanvasPanelSlot* S = Canvas->AddChildToCanvas(W);
	S->SetAnchors(FAnchors(Anchor.X, Anchor.Y));
	S->SetAlignment(Align);
	S->SetPosition(Pos);
	S->SetAutoSize(true);
	return S;
}

UButton* UCLHud::Click(ECLLab Which, UWidget* Content)
{
	UButton* B = WidgetTree->ConstructWidget<UButton>();
	FButtonStyle St;
	St.SetNormal(Rounded(FLinearColor(0, 0, 0, 0), 5.f));
	St.SetHovered(Rounded(FLinearColor(0.5f, 0.8f, 1.f, 0.10f), 5.f, Sky, 1.f));
	St.SetPressed(Rounded(FLinearColor(0.5f, 0.8f, 1.f, 0.22f), 5.f, Sky, 1.f));
	St.SetNormalPadding(FMargin(0)); St.SetPressedPadding(FMargin(0));
	B->SetStyle(St);
	UCLClick* H = NewObject<UCLClick>(this);
	H->Which = Which; H->Owner = PC();
	B->OnClicked.AddDynamic(H, &UCLClick::OnClicked);
	Clicks.Add(H);
	B->SetContent(Content);
	if (UButtonSlot* BS = Cast<UButtonSlot>(Content->Slot)) { BS->SetHorizontalAlignment(HAlign_Fill); BS->SetPadding(FMargin(0)); }
	return B;
}

void UCLHud::Group(UVerticalBox* Box, const FString& Title, const FString& Sub)
{
	UVerticalBox* V = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* A = Text(15, Sky, true); A->SetText(T(Title));
	V->AddChildToVerticalBox(A);
	if (!Sub.IsEmpty()) { UTextBlock* B = Text(11, Dim, false, 330.f); B->SetText(T(Sub)); V->AddChildToVerticalBox(B); }
	Box->AddChildToVerticalBox(V)->SetPadding(FMargin(2, 10, 0, 3));
}

void UCLHud::SubHead(UVerticalBox* Box, const FString& Title, int32 Indent)
{
	UHorizontalBox* H = WidgetTree->ConstructWidget<UHorizontalBox>();
	USpacer* Ind = WidgetTree->ConstructWidget<USpacer>(); Ind->SetSize(FVector2D(8.f + Indent * 22.f, 1));
	H->AddChildToHorizontalBox(Ind);
	UTextBlock* A = Text(11, Dim, true); A->SetText(T(Title));
	H->AddChildToHorizontalBox(A)->SetVerticalAlignment(VAlign_Center);
	Box->AddChildToVerticalBox(H)->SetPadding(FMargin(0, 6, 0, 1));
}

void UCLHud::Row(UVerticalBox* Box, ECLLab Which, bool bRadio, int32 Indent)
{
	UHorizontalBox* H = WidgetTree->ConstructWidget<UHorizontalBox>();
	USpacer* Ind = WidgetTree->ConstructWidget<USpacer>(); Ind->SetSize(FVector2D(6.f + Indent * 22.f, 1));
	H->AddChildToHorizontalBox(Ind);
	if (Indent > 0) { UTextBlock* Tree = Text(12, Dim); Tree->SetText(T(TEXT("└ "))); H->AddChildToHorizontalBox(Tree)->SetVerticalAlignment(VAlign_Center); }
	UBorder* Mark = Panel(FLinearColor::Transparent, Ink, bRadio ? 7.f : 2.5f, FMargin(0));
	USizeBox* MB = WidgetTree->ConstructWidget<USizeBox>(); MB->SetWidthOverride(14.f); MB->SetHeightOverride(14.f); MB->SetContent(Mark);
	H->AddChildToHorizontalBox(MB)->SetVerticalAlignment(VAlign_Center);
	USpacer* G = WidgetTree->ConstructWidget<USpacer>(); G->SetSize(FVector2D(8, 1));
	H->AddChildToHorizontalBox(G);
	UTextBlock* Name = Text(14, Ink); Name->SetText(ACLPlayerController::LabName(Which));
	H->AddChildToHorizontalBox(Name)->SetVerticalAlignment(VAlign_Center);
	USpacer* Fl = WidgetTree->ConstructWidget<USpacer>();
	H->AddChildToHorizontalBox(Fl)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	const FString Key = ACLPlayerController::LabKey(Which);
	if (!Key.IsEmpty()) H->AddChildToHorizontalBox(KeyCap(Key, 10))->SetVerticalAlignment(VAlign_Center);
	UBorder* Back = Panel(FLinearColor::Transparent, FLinearColor::Transparent, 5.f, FMargin(2, 3, 6, 3));
	Back->SetContent(H);
	Back->SetToolTipText(ACLPlayerController::LabHint(Which));
	Box->AddChildToVerticalBox(Click(Which, Back));
	Rows.Add({ Which, bRadio, Mark, Name });
}

UBorder* UCLHud::Bar(FLinearColor Col, float W, float H, TObjectPtr<UBorder>& OutFill)
{
	UBorder* Track = Panel(FLinearColor(0.14f, 0.17f, 0.2f, 0.95f), FLinearColor::Transparent, 3.f, FMargin(0));
	UHorizontalBox* TB = WidgetTree->ConstructWidget<UHorizontalBox>();
	OutFill = Panel(Col, FLinearColor::Transparent, 3.f, FMargin(0));
	USizeBox* FS = WidgetTree->ConstructWidget<USizeBox>(); FS->SetWidthOverride(1.f); FS->SetHeightOverride(H); FS->SetContent(OutFill);
	TB->AddChildToHorizontalBox(FS);
	USizeBox* TS = WidgetTree->ConstructWidget<USizeBox>(); TS->SetWidthOverride(W); TS->SetHeightOverride(H); TS->SetContent(TB);
	Track->SetContent(TS);
	Track->SetHorizontalAlignment(HAlign_Left);
	return Track;
}

void UCLHud::SetBar(UBorder* F, float Frac, float W)
{
	if (USizeBox* SB = Cast<USizeBox>(F->GetParent())) SB->SetWidthOverride(FMath::Max(1.f, W * FMath::Clamp(Frac, 0.f, 1.f)));
}

TSharedRef<SWidget> UCLHud::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = Canvas;
		auto VBox = [this]() { return WidgetTree->ConstructWidget<UVerticalBox>(); };
		auto HBox = [this]() { return WidgetTree->ConstructWidget<UHorizontalBox>(); };
		auto Label = [this](const FString& S, int32 Size, FLinearColor C, bool bBold = false, float Wrap = 0.f) { UTextBlock* Tx = Text(Size, C, bBold, Wrap); Tx->SetText(T(S)); return Tx; };
		auto Gap = [this](float W, float H) { USpacer* S = WidgetTree->ConstructWidget<USpacer>(); S->SetSize(FVector2D(W, H)); return S; };
		auto Sized = [this](UWidget* W, float Wd, float Ht) { USizeBox* B = WidgetTree->ConstructWidget<USizeBox>(); if (Wd > 0) B->SetWidthOverride(Wd); if (Ht > 0) B->SetHeightOverride(Ht); B->SetContent(W); return B; };

		// lightning flash (full screen, under everything)
		Flash = Panel(FLinearColor(0.85f, 0.9f, 1.f, 0.f), FLinearColor::Transparent, 0.f, FMargin(0));
		{ UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Flash); S->SetAnchors(FAnchors(0, 0, 1, 1)); S->SetOffsets(FMargin(0)); }
		Flash->SetVisibility(ESlateVisibility::HitTestInvisible);

		// markers projected into the view
		Reticle = Label(TEXT("◯"), 30, FLinearColor(1, 1, 1, 0.85f)); Put(Reticle, FVector2D(0, 0), FVector2D(0.5f, 0.5f), FVector2D(0, 0));
		Nose = Label(TEXT("✚"), 18, Good); Put(Nose, FVector2D(0, 0), FVector2D(0.5f, 0.5f), FVector2D(0, 0));
		RingMark = Label(TEXT(""), 14, Amber, true); Put(RingMark, FVector2D(0, 0), FVector2D(0.5f, 1.f), FVector2D(0, 0));
		RingArrow = Label(TEXT("➤"), 34, Amber, true); Put(RingArrow, FVector2D(0, 0), FVector2D(0.5f, 0.5f), FVector2D(0, 0));
		for (UWidget* W : { (UWidget*)Reticle, (UWidget*)Nose, (UWidget*)RingMark, (UWidget*)RingArrow }) W->SetVisibility(ESlateVisibility::HitTestInvisible);

		// ---------------- title + objective (top-left)
		{
			UBorder* Card = Panel(Fill, Rim, 9.f, FMargin(16, 11, 18, 12));
			UVerticalBox* V = VBox();
			V->AddChildToVerticalBox(Label(TEXT("穿云竞速"), 22, Ink, true));
			V->AddChildToVerticalBox(Label(TEXT("体素云 · SDF 跳空 · 光照网格 · 两档分辨率"), 12, Dim));
			V->AddChildToVerticalBox(Gap(1, 6));
			RingText = Text(14, Amber, true); V->AddChildToVerticalBox(RingText);
			Objective = Text(13, Ink, false, 330.f); V->AddChildToVerticalBox(Objective);
			Card->SetContent(V);
			Put(Card, FVector2D(0, 0), FVector2D(0, 0), FVector2D(18, 16));
		}
		// ---------------- clock (top centre)
		{
			UBorder* Card = Panel(Fill, Rim, 10.f, FMargin(22, 6, 22, 8));
			UVerticalBox* V = VBox();
			Clock = Text(30, Ink, true); Clock->SetJustification(ETextJustify::Center); V->AddChildToVerticalBox(Clock);
			BestText = Text(12, Dim); BestText->SetJustification(ETextJustify::Center); V->AddChildToVerticalBox(BestText);
			Card->SetContent(V);
			Put(Card, FVector2D(0.5f, 0), FVector2D(0.5f, 0), FVector2D(0, 14));
			EventText = Text(17, Amber, true); EventText->SetJustification(ETextJustify::Center);
			Put(EventText, FVector2D(0.5f, 0), FVector2D(0.5f, 0), FVector2D(0, 104));
		}
		// ---------------- render readouts (top-right)
		{
			UBorder* Card = Panel(FillSoft, Rim, 9.f, FMargin(14, 10, 16, 11));
			UVerticalBox* V = VBox();
			V->AddChildToVerticalBox(Label(TEXT("渲染读数"), 13, Sky, true));
			Readout = Text(12, Ink); V->AddChildToVerticalBox(Readout);
			Card->SetContent(Sized(V, 380.f, 0));
			Put(Card, FVector2D(1, 0), FVector2D(1, 0), FVector2D(-18, 16));
		}
		// ---------------- instruments (bottom centre)
		{
			UBorder* Card = Panel(Fill, Rim, 10.f, FMargin(18, 10, 20, 12));
			UHorizontalBox* H = HBox();
			auto Gauge = [&](const FString& Name, TObjectPtr<UTextBlock>& Val)
			{
				UVerticalBox* V = VBox();
				V->AddChildToVerticalBox(Label(Name, 11, Dim));
				Val = Text(24, Ink, true); V->AddChildToVerticalBox(Val);
				return V;
			};
			H->AddChildToHorizontalBox(Sized(Gauge(TEXT("空速 km/h"), SpeedText), 110.f, 0));
			H->AddChildToHorizontalBox(Sized(Gauge(TEXT("高度 m"), AltText), 110.f, 0));
			UVerticalBox* Th = VBox();
			Th->AddChildToVerticalBox(Label(TEXT("油门"), 11, Dim));
			Th->AddChildToVerticalBox(Bar(Sky, 150.f, 9.f, ThrottleFill))->SetPadding(FMargin(0, 6, 0, 4));
			UHorizontalBox* K = HBox();
			K->AddChildToHorizontalBox(KeyCap(TEXT("W"), 11)); K->AddChildToHorizontalBox(Label(TEXT(" 加  "), 12, Dim));
			K->AddChildToHorizontalBox(KeyCap(TEXT("S"), 11)); K->AddChildToHorizontalBox(Label(TEXT(" 减"), 12, Dim));
			Th->AddChildToVerticalBox(K);
			H->AddChildToHorizontalBox(Th);
			H->AddChildToHorizontalBox(Gap(24, 1));
			UVerticalBox* Ic = VBox();
			IceText = Text(11, Dim); Ic->AddChildToVerticalBox(IceText);
			Ic->AddChildToVerticalBox(Bar(Ice, 150.f, 9.f, IceFill))->SetPadding(FMargin(0, 6, 0, 4));
			Alert = Text(14, Bad, true); Ic->AddChildToVerticalBox(Alert);
			H->AddChildToHorizontalBox(Ic);
			Card->SetContent(H);
			Put(Card, FVector2D(0.5f, 1), FVector2D(0.5f, 1), FVector2D(0, -18));
		}
		// ---------------- help (bottom-left)
		{
			HelpCard = Panel(FillSoft, Rim, 9.f, FMargin(14, 9, 16, 10));
			UVerticalBox* V = VBox();
			auto KeyRow = [&](const FString& Kk, const FString& What)
			{
				UHorizontalBox* R = HBox();
				R->AddChildToHorizontalBox(Sized(KeyCap(Kk, 11), 72.f, 0));
				R->AddChildToHorizontalBox(Label(What, 13, Ink));
				V->AddChildToVerticalBox(R)->SetPadding(FMargin(0, 1, 0, 1));
			};
			KeyRow(TEXT("鼠标"), TEXT("指向要飞去的方向，飞机自动压坡度转弯"));
			KeyRow(TEXT("W / S"), TEXT("油门加 / 减"));
			KeyRow(TEXT("A / D"), TEXT("额外横滚"));
			KeyRow(TEXT("C"), TEXT("追尾 / 座舱视角"));
			KeyRow(TEXT("Tab"), TEXT("原理面板（鼠标点选开关）"));
			KeyRow(TEXT("V"), TEXT("切换调试视图"));
			KeyRow(TEXT("R"), TEXT("回到上一个检查环（罚时 5 秒）"));
			KeyRow(TEXT("Enter"), TEXT("重新开始"));
			KeyRow(TEXT("F1"), TEXT("隐藏这张说明"));
			HelpCard->SetContent(V);
			Put(HelpCard, FVector2D(0, 1), FVector2D(0, 1), FVector2D(18, -18));
		}
		// ---------------- banner
		{
			Banner = Panel(Fill, Amber, 10.f, FMargin(26, 16, 26, 18));
			UVerticalBox* V = VBox();
			BannerTitle = Text(30, Amber, true); BannerTitle->SetJustification(ETextJustify::Center);
			BannerBody = Text(15, Ink, false, 520.f); BannerBody->SetJustification(ETextJustify::Center);
			V->AddChildToVerticalBox(BannerTitle);
			V->AddChildToVerticalBox(BannerBody)->SetPadding(FMargin(0, 6, 0, 0));
			Banner->SetContent(V);
			Put(Banner, FVector2D(0.5f, 0.32f), FVector2D(0.5f, 0.5f), FVector2D(0, 0));
		}
		// ---------------- loading
		{
			Loading = Panel(Fill, Sky, 10.f, FMargin(26, 16, 26, 18));
			LoadingText = Text(16, Ink, false, 520.f);
			Loading->SetContent(LoadingText);
			Put(Loading, FVector2D(0.5f, 0.5f), FVector2D(0.5f, 0.5f), FVector2D(0, 0));
		}
		// ---------------- principle panel (left, Tab)
		{
			LabPanel = Panel(Fill, Rim, 10.f, FMargin(12, 10, 10, 12));
			UVerticalBox* Outer = VBox();
			UHorizontalBox* Head = HBox();
			Head->AddChildToHorizontalBox(Label(TEXT("原理面板"), 19, Ink, true));
			Head->AddChildToHorizontalBox(Gap(10, 1));
			Head->AddChildToHorizontalBox(Label(TEXT("圆点互斥 · 方框叠加 · 缩进项从属于上一行"), 11, Dim))->SetVerticalAlignment(VAlign_Bottom);
			Outer->AddChildToVerticalBox(Head);
			UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
			UVerticalBox* B = VBox();
			Group(B, TEXT("① 渲染路线"), TEXT("整套替换：同一份体素数据交给谁来画"));
			Row(B, ECLLab::RouteCustom, true, 0); Row(B, ECLLab::RouteUE, true, 0);
			Group(B, TEXT("② 云的表示"), TEXT("同一片云景存成三维体素，或只存每列的最低/最高高度"));
			Row(B, ECLLab::RepVoxel, true, 0); Row(B, ECLLab::RepEnvelope, true, 0);
			Group(B, TEXT("③ 细节"), TEXT("8 m 的维度轮廓在步进时被噪声提升到约 0.5 m 的细节"));
			Row(B, ECLLab::Uprez, false, 0); Row(B, ECLLab::NearFold, false, 1); Row(B, ECLLab::NoiseMip, false, 1);
			Group(B, TEXT("④ 光线步进"), TEXT("采样放在哪里、走多少步"));
			SubHead(B, TEXT("步长 · 二选一"), 0);
			Row(B, ECLLab::StepLegacy, true, 0); Row(B, ECLLab::StepAdaptive, true, 0);
			SubHead(B, TEXT("叠加在步长之上"), 0);
			Row(B, ECLLab::SdfSkip, false, 0);
			SubHead(B, TEXT("SDF 编码 · 四选一"), 1);
			Row(B, ECLLab::SdfR16, true, 1); Row(B, ECLLab::SdfBC1, true, 1); Row(B, ECLLab::SdfBC1Naive, true, 1); Row(B, ECLLab::SdfBC4, true, 1);
			SubHead(B, TEXT("步数上限 · 四选一"), 0);
			Row(B, ECLLab::Cap128, true, 0); Row(B, ECLLab::Cap256, true, 0); Row(B, ECLLab::Cap512, true, 0); Row(B, ECLLab::CapNone, true, 0);
			Group(B, TEXT("⑤ 光照"), TEXT("太阳光的二次步进最贵：拆出去预计算"));
			SubHead(B, TEXT("太阳光 · 二选一"), 0);
			Row(B, ECLLab::LightMarch, true, 0); Row(B, ECLLab::LightGrid, true, 0);
			SubHead(B, TEXT("可叠加的光照项"), 0);
			Row(B, ECLLab::MultiScatter, false, 0); Row(B, ECLLab::Ambient, false, 0); Row(B, ECLLab::AmbientDir, false, 1); Row(B, ECLLab::Lightning, false, 0);
			Group(B, TEXT("⑥ 步进分辨率"), TEXT("三选一"));
			Row(B, ECLLab::ResFull, true, 0); Row(B, ECLLab::ResHalf, true, 0); Row(B, ECLLab::ResSplit, true, 0);
			Group(B, TEXT("⑦ 调试视图"), TEXT("六选一（V 键循环）"));
			Row(B, ECLLab::ViewNormal, true, 0); Row(B, ECLLab::ViewIters, true, 0); Row(B, ECLLab::ViewSamples, true, 0);
			Row(B, ECLLab::ViewLight, true, 0); Row(B, ECLLab::ViewSplit, true, 0); Row(B, ECLLab::ViewOver, true, 0);
			Group(B, TEXT("⑧ 时段"), TEXT("三选一：太阳一动，光照网格整张重算"));
			Row(B, ECLLab::TodMorning, true, 0); Row(B, ECLLab::TodNoon, true, 0); Row(B, ECLLab::TodEvening, true, 0);
			Group(B, TEXT("操作"), TEXT(""));
			Row(B, ECLLab::Restart, false, 0); Row(B, ECLLab::Respawn, false, 0); Row(B, ECLLab::CockpitCam, false, 0);
			Scroll->AddChild(B);
			Outer->AddChildToVerticalBox(Sized(Scroll, 372.f, 700.f))->SetPadding(FMargin(0, 6, 0, 0));
			LabPanel->SetContent(Outer);
			Put(LabPanel, FVector2D(0, 0), FVector2D(0, 0), FVector2D(18, 116));
		}
	}
	return Super::RebuildWidget();
}

void UCLHud::NativeTick(const FGeometry& Geo, float Dt)
{
	Super::NativeTick(Geo, Dt);
	ACLPlayerController* P = PC();
	if (!P) return;
	ACLPlane* Pl = P->GetPlane();
	ACLCloudManager* M = ACLCloudManager::Get(P);
	ACLRace* R = ACLRace::Get(P);
	const float Now = P->GetWorld()->GetTimeSeconds();
	const FVector2D Size = Geo.GetLocalSize();

	// ---- projected markers (every frame)
	auto Place = [&](UWidget* W, FVector2D Pos) { if (UCanvasPanelSlot* S = Cast<UCanvasPanelSlot>(W->Slot)) S->SetPosition(Pos); };
	bool bShowMarks = Pl && R && R->State == ECLRaceState::Flying;
	if (Pl)
	{
		FVector2D Sp;
		if (!Pl->bCockpitCam && UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(P, Pl->GetActorLocation() + Pl->Aim.Vector() * 30000.f, Sp, false)) Place(Reticle, Sp);
		Reticle->SetVisibility(Pl->bCockpitCam ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
		if (UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(P, Pl->GetActorLocation() + Pl->NoseDir() * 30000.f, Sp, false)) Place(Nose, Sp);
	}
	RingMark->SetVisibility(ESlateVisibility::Collapsed);
	RingArrow->SetVisibility(ESlateVisibility::Collapsed);
	if (bShowMarks && R->Rings.IsValidIndex(R->Next))
	{
		const FVector RingP = R->Rings[R->Next]->GetActorLocation();
		const float Dist = FVector::Dist(RingP, Pl->GetActorLocation()) * 0.01f;
		FVector2D Sp;
		const FVector CamFwd = P->PlayerCameraManager->GetCameraRotation().Vector();
		const FVector ToRing = RingP - P->PlayerCameraManager->GetCameraLocation();
		const bool bFront = FVector::DotProduct(CamFwd, ToRing) > 0.f;
		const bool bOn = bFront && UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(P, RingP, Sp, false) && Sp.X > 30 && Sp.Y > 30 && Sp.X < Size.X - 30 && Sp.Y < Size.Y - 30;
		if (bOn)
		{
			RingMark->SetText(T(FString::Printf(TEXT("◇ %s  %.0f m"), *R->Rings[R->Next]->Label, Dist)));
			Place(RingMark, Sp - FVector2D(0, R->Rings[R->Next]->RadiusCm * 0.f + 8.f));
			RingMark->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			// arrow on the screen edge pointing toward the ring
			const FVector L = P->PlayerCameraManager->GetCameraRotation().UnrotateVector(ToRing);
			FVector2D D(L.Y, -L.Z); D = D.GetSafeNormal();
			if (D.IsNearlyZero()) D = FVector2D(0, -1);
			const FVector2D C = Size * 0.5f;
			const float K = FMath::Min((Size.X * 0.5f - 60.f) / FMath::Max(FMath::Abs(D.X), 1e-3f), (Size.Y * 0.5f - 60.f) / FMath::Max(FMath::Abs(D.Y), 1e-3f));
			Place(RingArrow, C + D * K);
			RingArrow->SetRenderTransformAngle(FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X)));
			RingArrow->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
	}
	// lightning flash on screen when it strikes close to the plane
	if (M && Pl)
	{
		const float Age = Now - M->LastStrikeTime;
		const float D = FVector::Dist(M->LastStrikePos, Pl->GetActorLocation()) * 0.01f;
		const float A = Age < 0.5f ? FMath::Exp(-Age * 8.f) * FMath::Clamp(1.f - D / 900.f, 0.f, 1.f) * 0.55f : 0.f;
		Flash->SetBrushColor(FLinearColor(0.85f, 0.9f, 1.f, A));
	}

	Refresh -= Dt;
	if (Refresh > 0.f) return;
	Refresh = 0.08f;

	LabPanel->SetVisibility(P->IsPanelOpen() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	// the panel takes the left column; the mission card keeps only its title and ring line
	Objective->SetVisibility(P->IsPanelOpen() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	HelpCard->SetVisibility(P->IsHelpOpen() && !P->IsPanelOpen() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	for (const FRowW& Rw : Rows)
	{
		const bool bOn = P->LabOn(Rw.Which), bEn = P->LabEnabled(Rw.Which);
		Rw.Mark->SetBrush(Rounded(bOn ? (bEn ? Sky : Off) : FLinearColor::Transparent, Rw.bRadio ? 7.f : 2.5f, bEn ? Ink : Off, 1.5f));
		Rw.Name->SetColorAndOpacity(FSlateColor(bEn ? (bOn ? Ink : Dim) : Off));
	}

	const bool bReady = M && M->IsReady();
	Loading->SetVisibility(bReady ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	if (!bReady) LoadingText->SetText(T(TEXT("正在生成云景……\n积云与雷暴云的体素化、精确距离变换（SDF）、2.5D 包络、BC1 / BC4 保守编码和 128³ 噪声，约需十几秒。")));

	if (R)
	{
		const int32 N = R->Rings.Num();
		RingText->SetText(T(N ? FString::Printf(TEXT("检查环 %d / %d%s"), FMath::Min(R->Next, N), N, R->Rings.IsValidIndex(R->Next) ? *FString::Printf(TEXT(" · 下一个：%s"), *R->Rings[R->Next]->Label) : TEXT("")) : TEXT("")));
		Objective->SetText(T(TEXT("按顺序穿过橙色检查环：先钻过隧道云，绕过两座云塔，飞进云洞，从层云底下掠过，最后穿过雷暴云里的巢穴。在浓云里待久了会结冰掉速，隧道里的空气是干净的。")));
		Clock->SetText(T(Mmss(R->RaceSeconds())));
		BestText->SetText(T(R->Best > 0.f ? FString::Printf(TEXT("最好成绩 %s%s"), *Mmss(R->Best), R->Penalty > 0 ? *FString::Printf(TEXT(" · 本次罚时 %.0f 秒"), R->Penalty) : TEXT(""))
			: (R->Penalty > 0 ? FString::Printf(TEXT("本次罚时 %.0f 秒"), R->Penalty) : FString(TEXT("计时赛")))));
		EventText->SetText(T(Now - R->LastEventTime < 2.5f ? R->LastEvent : FString()));
		switch (R->bCaptureHold ? ECLRaceState::Flying : R->State)
		{
		case ECLRaceState::Countdown:
			Banner->SetVisibility(ESlateVisibility::HitTestInvisible);
			BannerTitle->SetText(T(FString::Printf(TEXT("%d"), FMath::Max(1, 3 - int32(Now - R->StateTime)))));
			BannerTitle->SetColorAndOpacity(FSlateColor(Amber));
			BannerBody->SetText(T(TEXT("用鼠标指向第一个检查环。")));
			break;
		case ECLRaceState::Finished:
			Banner->SetVisibility(ESlateVisibility::HitTestInvisible);
			BannerTitle->SetText(T(TEXT("完成！")));
			BannerTitle->SetColorAndOpacity(FSlateColor(Good));
			BannerBody->SetText(T(FString::Printf(TEXT("用时 %s（含罚时 %.0f 秒，坠毁 %d 次）。最好成绩 %s。\n按 Enter 再飞一次；打开 Tab 面板换成 2.5D 包络，看看隧道还在不在。"), *Mmss(R->RaceSeconds()), R->Penalty, R->Crashes, *Mmss(R->Best))));
			break;
		case ECLRaceState::Crashed:
			Banner->SetVisibility(ESlateVisibility::HitTestInvisible);
			BannerTitle->SetText(T(R->LastEvent));
			BannerTitle->SetColorAndOpacity(FSlateColor(Bad));
			BannerBody->SetText(T(TEXT("按 R 从上一个检查环继续（罚时 5 秒），或按 Enter 从头再来。")));
			break;
		default: Banner->SetVisibility(ESlateVisibility::Collapsed); break;
		}
	}

	if (Pl)
	{
		SpeedText->SetText(T(FString::Printf(TEXT("%d"), FMath::RoundToInt(Pl->SpeedMs * 3.6f))));
		AltText->SetText(T(FString::Printf(TEXT("%d"), FMath::RoundToInt(Pl->AltitudeM()))));
		SetBar(ThrottleFill, Pl->Throttle, 150.f);
		SetBar(IceFill, Pl->Ice, 150.f);
		IceText->SetText(T(FString::Printf(TEXT("机翼结冰 %d%%"), FMath::RoundToInt(Pl->Ice * 100.f))));
		FString Al;
		if (Pl->bStalled) Al = TEXT("结冰失速！飞出云层除冰");
		else if (Pl->ShockT > 0.f) Al = TEXT("雷击！仪表受扰");
		else if (Pl->Ice > 0.5f) Al = TEXT("结冰严重");
		else if (Pl->CloudDensity > 0.04f) Al = TEXT("云中 · 正在结冰");
		else if (Pl->HeightAboveGroundM() < 60.f) Al = TEXT("高度过低");
		Alert->SetText(T(Al));
		Alert->SetColorAndOpacity(FSlateColor(Pl->bStalled || Pl->ShockT > 0.f || Pl->HeightAboveGroundM() < 60.f ? Bad : Ice));
	}

	if (M)
	{
		const FCLGpuTimes& G = M->Gpu;
		const FCLRenderSettings& Rs = M->Render;
		static const TCHAR* Fmt[] = { TEXT("R16"), TEXT("BC1 保守"), TEXT("BC1 不保守"), TEXT("BC4 对数") };
		static const TCHAR* Res[] = { TEXT("全分辨率"), TEXT("半分辨率"), TEXT("两档拆分") };
		FString S;
		if (M->Route == ECLRoute::UECloud) S = TEXT("UE 体积云组件（同一份体素数据）\n耗时见 stat gpu 的 VolumetricCloud 项");
		else
		{
			S = FString::Printf(TEXT("%s · %s · %s · %s\n"), Rs.Rep == ECLRepresentation::Voxel ? TEXT("三维体素") : TEXT("2.5D 包络"),
				Rs.bSdfSkip ? *FString::Printf(TEXT("SDF %s"), Fmt[(int32)Rs.SdfFormat]) : TEXT("无 SDF"), Rs.Lighting == 1 ? TEXT("光照网格") : TEXT("光照步进"), Res[FMath::Clamp(Rs.Resolution, 0, 2)]);
			S += FString::Printf(TEXT("光照网格 %.2f ms（%d 层）\n"), G.LightMs, G.LightSlices);
			if (G.NearSize.X > 0) S += FString::Printf(TEXT("近层 %.2f ms  %d×%d\n"), G.NearMs, G.NearSize.X, G.NearSize.Y);
			S += FString::Printf(TEXT("远层 %.2f ms  %d×%d\n合成 %.2f ms · 云合计 %.2f ms\n"), G.FarMs, G.FarSize.X, G.FarSize.Y, G.CompositeMs, G.LightMs + G.NearMs + G.FarMs + G.CompositeMs);
		}
		S += FString::Printf(TEXT("帧时间 %.1f ms"), P->FrameMs);
		if (Pl && bReady) S += FString::Printf(TEXT("\n飞机处：云密度 %.2f · 距最近的云 %.0f m"), Pl->CloudDensity, FMath::Max(0.f, M->SdfAt(Pl->GetActorLocation())));
		if (bReady && M->GetField())
		{
			const FCLCloudField::FStats& St = M->GetField()->Stats;
			S += FString::Printf(TEXT("\n云景生成 %.1f s · 体素占用 %.1f%% · 包络占用 %.1f%%"), M->BuildSeconds, St.Occupancy * 100.f, St.EnvelopeOccupancy * 100.f);
		}
		Readout->SetText(T(S));
	}
}
