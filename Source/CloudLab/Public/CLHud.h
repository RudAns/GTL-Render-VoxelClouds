#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CLPlayerController.h"
#include "CLHud.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UCanvasPanelSlot;
class UTextBlock;
class UVerticalBox;
class USizeBox;

UCLASS()
class UCLClick : public UObject
{
	GENERATED_BODY()
public:
	ECLLab Which = ECLLab::Count;
	TWeakObjectPtr<ACLPlayerController> Owner;
	UFUNCTION() void OnClicked();
};

/** Game HUD and the principle panel, built in code (UMG). */
UCLASS()
class UCLHud : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& Geo, float Dt) override;

private:
	ACLPlayerController* PC() const;
	UTextBlock* Text(int32 Size, FLinearColor Color, bool bBold = false, float Wrap = 0.f);
	UBorder* Panel(FLinearColor Fill, FLinearColor Rim, float Radius, FMargin Pad);
	UBorder* KeyCap(const FString& Key, int32 Size = 12);
	UCanvasPanelSlot* Put(UWidget* W, FVector2D Anchor, FVector2D Align, FVector2D Pos);
	void Group(UVerticalBox* Box, const FString& Title, const FString& Sub);
	void SubHead(UVerticalBox* Box, const FString& Title, int32 Indent);
	void Row(UVerticalBox* Box, ECLLab Which, bool bRadio, int32 Indent);
	UButton* Click(ECLLab Which, UWidget* Content);
	UBorder* Bar(FLinearColor Col, float W, float H, TObjectPtr<UBorder>& OutFill);
	void SetBar(UBorder* Fill, float Frac, float W);

	UPROPERTY() TObjectPtr<UCanvasPanel> Canvas;
	UPROPERTY() TArray<TObjectPtr<UCLClick>> Clicks;
	UPROPERTY() TObjectPtr<UBorder> LabPanel;
	struct FRowW { ECLLab Which; bool bRadio; TObjectPtr<UBorder> Mark; TObjectPtr<UTextBlock> Name; };
	TArray<FRowW> Rows;

	UPROPERTY() TObjectPtr<UTextBlock> Objective;
	UPROPERTY() TObjectPtr<UTextBlock> RingText;
	UPROPERTY() TObjectPtr<UTextBlock> Clock;
	UPROPERTY() TObjectPtr<UTextBlock> BestText;
	UPROPERTY() TObjectPtr<UTextBlock> EventText;
	UPROPERTY() TObjectPtr<UTextBlock> Readout;
	UPROPERTY() TObjectPtr<UTextBlock> SpeedText;
	UPROPERTY() TObjectPtr<UTextBlock> AltText;
	UPROPERTY() TObjectPtr<UBorder> ThrottleFill;
	UPROPERTY() TObjectPtr<UBorder> IceFill;
	UPROPERTY() TObjectPtr<UTextBlock> IceText;
	UPROPERTY() TObjectPtr<UTextBlock> Alert;
	UPROPERTY() TObjectPtr<UBorder> Banner;
	UPROPERTY() TObjectPtr<UTextBlock> BannerTitle;
	UPROPERTY() TObjectPtr<UTextBlock> BannerBody;
	UPROPERTY() TObjectPtr<UBorder> HelpCard;
	UPROPERTY() TObjectPtr<UBorder> Loading;
	UPROPERTY() TObjectPtr<UTextBlock> LoadingText;
	UPROPERTY() TObjectPtr<UTextBlock> Reticle;
	UPROPERTY() TObjectPtr<UTextBlock> Nose;
	UPROPERTY() TObjectPtr<UTextBlock> RingMark;
	UPROPERTY() TObjectPtr<UTextBlock> RingArrow;
	UPROPERTY() TObjectPtr<UBorder> Flash;
	float Refresh = 0.f;
};
