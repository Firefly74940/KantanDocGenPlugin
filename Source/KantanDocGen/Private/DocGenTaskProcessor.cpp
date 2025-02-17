// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Copyright (C) 2016-2017 Cameron Angus. All Rights Reserved.

#include "DocGenTaskProcessor.h"
#include "KantanDocGenLog.h"
#include "NodeDocsGenerator.h"
#if WITH_EDITOR
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "K2Node.h"
#include "Enumeration/ISourceObjectEnumerator.h"
#include "Enumeration/NativeModuleEnumerator.h"
#include "Enumeration/ContentPathEnumerator.h"
#include "Enumeration/CompositeEnumerator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#endif

#include "ThreadingHelpers.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Private/ClassViewerNode.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistryModule.h"
#include "Animation/AnimBlueprint.h"
#include "ARFilter.h"


#define LOCTEXT_NAMESPACE "KantanDocGen"


FDocGenTaskProcessor::FDocGenTaskProcessor()
{
	bRunning = false;
	bTerminationRequest = false;
}

void FDocGenTaskProcessor::QueueTask(FKantanDocGenSettings const& Settings)
{
	TSharedPtr< FDocGenTask > NewTask = MakeShareable(new FDocGenTask());
	NewTask->Settings = Settings;

	FNotificationInfo Info(LOCTEXT("DocGenWaiting", "Doc gen waiting"));
	Info.Image = nullptr;//FEditorStyle::GetBrush(TEXT("LevelEditor.RecompileGameCode"));
	Info.FadeInDuration = 0.2f;
	Info.ExpireDuration = 5.0f;
	Info.FadeOutDuration = 1.0f;
	Info.bUseThrobber = true;
	Info.bUseSuccessFailIcons = true;
	Info.bUseLargeFont = true;
	Info.bFireAndForget = false;
	Info.bAllowThrottleWhenFrameRateIsLow = false;
	NewTask->Notification = FSlateNotificationManager::Get().AddNotification(Info);
	NewTask->Notification->SetCompletionState(SNotificationItem::CS_Pending);

	Waiting.Enqueue(NewTask);
}

bool FDocGenTaskProcessor::IsRunning() const
{
	return bRunning;
}

bool FDocGenTaskProcessor::Init()
{
	bRunning = true;
	return true;
}

uint32 FDocGenTaskProcessor::Run()
{
	TSharedPtr< FDocGenTask > Next;
	while (!bTerminationRequest && Waiting.Dequeue(Next))
	{
		ProcessTask(Next);
	}

	return 0;
}

void FDocGenTaskProcessor::Exit()
{
	bRunning = false;
}

void FDocGenTaskProcessor::Stop()
{
	bTerminationRequest = true;
}

void FDocGenTaskProcessor::ProcessTask(TSharedPtr< FDocGenTask > InTask)
{
	/********** Lambdas for the game thread to execute **********/

	const auto GameThread_InitDocGen = [this](FString const& DocTitle, FString const& IntermediateDir) -> bool
	{
		return Current->DocGen->GT_Init(DocTitle, IntermediateDir, Current->Task->Settings.BlueprintContextClass);
	};

	const auto GameThread_EnumerateNextObject = [this]() -> bool
	{
		Current->SourceObject.Reset();
		Current->CurrentSpawners.Empty();

		while (auto Obj = Current->CurrentEnumerator->GetNext())
		{
			// Ignore if already processed
			if (Current->Processed.Contains(Obj))
			{
				continue;
			}

			if (auto Class = Cast<UClass>(Obj))
			{
				ClassToPropertiesProcess.Add(Class);
			}
			else if (auto Blueprint = Cast<UBlueprint>(Obj))
			{
				ClassToPropertiesProcess.Add(Blueprint->GeneratedClass);
			}

			// Cache list of spawners for this object
			auto& BPActionMap = FBlueprintActionDatabase::Get().GetAllActions(); //@TODO Sidney Firefly
			if (auto ActionList = BPActionMap.Find(Obj))
			{
				if (ActionList->Num() == 0)
				{
					continue;
				}

				Current->SourceObject = Obj;
				for (auto Spawner : *ActionList)
				{
					// Add to queue as weak ptr
					check(Current->CurrentSpawners.Enqueue(Spawner));
				}

				// Done
				Current->Processed.Add(Obj);
				return true;
			}
		}

		// This enumerator is finished
		return false;
	};

	const auto GameThread_EnumerateNextNode = [this](FNodeDocsGenerator::FNodeProcessingState& OutState) -> UK2Node*
	{
		// We've just come in from another thread, check the source object is still around
		if (!Current->SourceObject.IsValid())
		{
			UE_LOG(LogKantanDocGen, Warning, TEXT("Object being enumerated expired!"));
			return nullptr;
		}

		// Try to grab the next spawner in the cached list
		TWeakObjectPtr< UBlueprintNodeSpawner > Spawner;
		while (Current->CurrentSpawners.Dequeue(Spawner))
		{
			if (Spawner.IsValid())
			{
				// See if we can document this spawner
				auto K2_NodeInst = Current->DocGen->GT_InitializeForSpawner(Spawner.Get(), Current->SourceObject.Get(), OutState);

				if (K2_NodeInst == nullptr)
				{
					continue;
				}

				// Make sure this node object will never be GCd until we're done with it.
				K2_NodeInst->AddToRoot();
				return K2_NodeInst;
			}
		}

		// No spawners left in the queue
		return nullptr;
	};

	const auto GameThread_FinalizeDocs = [this](FString const& OutputPath) -> bool
	{
		return Current->DocGen->GT_Finalize(OutputPath);
	};
	
	const auto GameThread_PreLoadPackages = [this]() -> void
	{
		// @TODO: Specific class enumerator
		Current->Enumerators.Enqueue(MakeShareable< FCompositeEnumerator< FNativeModuleEnumerator > >(new FCompositeEnumerator< FNativeModuleEnumerator >(Current->Task->Settings.NativeModules)));

		//Current->Enumerators.Enqueue(MakeShareable< FCompositeEnumerator< FContentPathEnumerator > >(new FCompositeEnumerator< FContentPathEnumerator >(Current->Task->Settings.ContentPaths)));
		TArray< FName > ContentPackagePaths;
		for (auto const& Path : Current->Task->Settings.ContentPaths)
		{
			ContentPackagePaths.AddUnique(FName(*Path.Path));
			//*FPackageName::FilenameToLongPackageName(RawPath.Path)));
		}
		Current->Enumerators.Enqueue(MakeShareable< FCompositeEnumerator< FContentPathEnumerator > >(new FCompositeEnumerator< FContentPathEnumerator >(ContentPackagePaths)));

	};

	/*****************************/


	Current = MakeUnique< FDocGenCurrentTask >();
	Current->Task = InTask;

	FString IntermediateDir = FPaths::ProjectIntermediateDir() / TEXT("KantanDocGen") / Current->Task->Settings.DocumentationTitle;


	DocGenThreads::RunOnGameThread(GameThread_PreLoadPackages);

	// Initialize the doc generator
	Current->DocGen = MakeUnique< FNodeDocsGenerator >();

	if (!DocGenThreads::RunOnGameThreadRetVal(GameThread_InitDocGen, Current->Task->Settings.DocumentationTitle, IntermediateDir))
	{
		UE_LOG(LogKantanDocGen, Error, TEXT("Failed to initialize doc generator!"));
		return;
	}

	Current->Task->Notification->SetExpireDuration(2.0f);
	Current->Task->Notification->SetText(LOCTEXT("DocGenInProgress", "Doc gen in progress"));

	bool const bCleanIntermediate = true;
	if (bCleanIntermediate)
	{
		IFileManager::Get().DeleteDirectory(*IntermediateDir, false, true);
	}

	for (auto const& Name : Current->Task->Settings.ExcludedClasses)
	{
		Current->Excluded.Add(Name);
	}

	int SuccessfulNodeCount = 0;
	while (Current->Enumerators.Dequeue(Current->CurrentEnumerator))
	{
		while (DocGenThreads::RunOnGameThreadRetVal(GameThread_EnumerateNextObject))	// Game thread: Enumerate next Obj, get spawner list for Obj, store as array of weak ptrs.
		{
			if (bTerminationRequest)
			{
				return;
			}

			FNodeDocsGenerator::FNodeProcessingState NodeState;
			while (auto NodeInst = DocGenThreads::RunOnGameThreadRetVal(GameThread_EnumerateNextNode, NodeState))	// Game thread: Get next still valid spawner, spawn node, add to root, return it)
			{
				// NodeInst should hopefully not reference anything except stuff we control (ie graph object), and it's rooted so should be safe to deal with here

				// Generate image
				if (!Current->DocGen->GenerateNodeImage(NodeInst, NodeState))
				{
					UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to generate node image!"))
						continue;
				}

				// Generate doc
				if (!Current->DocGen->GenerateNodeDocs(NodeInst, NodeState))
				{
					UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to generate node doc xml!"))
						continue;
				}

				++SuccessfulNodeCount;
			}
		}
	}

	if (SuccessfulNodeCount == 0)
	{
		UE_LOG(LogKantanDocGen, Error, TEXT("No nodes were found to document!"));

		Current->Task->Notification->SetText(LOCTEXT("DocFinalizationFailed", "Doc gen failed - No nodes found"));
		Current->Task->Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Current->Task->Notification->ExpireAndFadeout();
		//GEditor->PlayEditorSound(CompileSuccessSound);
		return;
	}
	//
	// properties class crawler 
	//

	for(auto Class : ClassToPropertiesProcess)
		Current->DocGen->AddClassIfNeeded(Class);
	/*
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		// Retrieve all blueprint classes 
		TArray<FAssetData> BlueprintList;

		FARFilter Filter;
		//Filter.ClassNames.Add(UClass::StaticClass()->GetFName());
		Filter.ClassNames.Add(UObject::StaticClass()->GetFName());
		//Filter.ClassNames.Add(UAnimBlueprint::StaticClass()->GetFName());
		//Filter.ClassNames.Add(UBlueprintGeneratedClass::StaticClass()->GetFName());

		//Include any Blueprint based objects as well, this includes things like Blutilities, UMG, and GameplayAbility objects
		Filter.bRecursiveClasses = true;
		AssetRegistryModule.Get().GetAssets(Filter, BlueprintList);//ByClass("Actor", BlueprintList, true);// GetAllAssets(BlueprintList);// GetAssets(Filter, BlueprintList);
		//TArray<FAssetData> BlueprintList2;
		//AssetRegistryModule.Get().GetAssetsByClass("ActorComponent", BlueprintList2);//ByClass("Actor", BlueprintList, true);// GetAllAssets(BlueprintList);// GetAssets(Filter, BlueprintList);

		TMultiMap<FName, FAssetData> BlueprintPackageToAssetDataMap;
		for (int32 AssetIdx = 0; AssetIdx < BlueprintList.Num(); ++AssetIdx)
		{
			const FString AssetName = BlueprintList[AssetIdx].AssetName.ToString();
			auto GeneratedClassPackage = BlueprintList[AssetIdx].PackageName.ToString();

			UPackage* Package = FindPackage(NULL, *GeneratedClassPackage);
			if (Package)
			{
				UObject* Object = FindObject<UObject>(Package, *AssetName);

				if (Object)
				{
					// Check if this item is a blueprint.
					if (Object->IsA(UBlueprint::StaticClass()))
					{
						auto Blueprint = Cast<UBlueprint>(Object);
						Current->DocGen->AddClassIfNeeded(Blueprint->GeneratedClass);
					}
				}
				else if (UClass * Class = Cast<UClass>(Object))
				{
					Current->DocGen->AddClassIfNeeded(Class);
				}
				else
				{
					Current->DocGen->AddClassIfNeeded(Object->GetClass());
				}


			}


		}

	}*/

	Current->DocGen->GT_GenerateProperties(Current->Task->Settings.bGenerateComponentsPropertiesForBlueprints);

	// Game thread: DocGen.GT_Finalize()
	if (!DocGenThreads::RunOnGameThreadRetVal(GameThread_FinalizeDocs, IntermediateDir))
	{
		UE_LOG(LogKantanDocGen, Error, TEXT("Failed to finalize xml docs!"));

		Current->Task->Notification->SetText(LOCTEXT("DocFinalizationFailed", "Doc gen failed"));
		Current->Task->Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Current->Task->Notification->ExpireAndFadeout();
		//GEditor->PlayEditorSound(CompileSuccessSound);
		return;
	}

	Current->Task->Notification->SetText(LOCTEXT("DocConversionInProgress", "Converting docs"));

	auto TransformationResult = ProcessIntermediateDocs(
		IntermediateDir,
		Current->Task->Settings.OutputDirectory.Path,
		Current->Task->Settings.DocumentationTitle,
		Current->Task->Settings.bCleanOutputDirectory
	);
	if (TransformationResult != EIntermediateProcessingResult::Success)
	{
		UE_LOG(LogKantanDocGen, Error, TEXT("Failed to transform xml to html!"));

		auto Msg = FText::Format(LOCTEXT("DocConversionFailed", "Doc gen failed - {0}"),
			TransformationResult == EIntermediateProcessingResult::DiskWriteFailure ? LOCTEXT("CouldNotWriteToOutput", "Could not write output, please clear output directory or enable 'Clean Output Directory' option") : LOCTEXT("GenericTransformationFailure", "Conversion failure")
		);
		Current->Task->Notification->SetText(Msg);
		Current->Task->Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Current->Task->Notification->ExpireAndFadeout();
		//GEditor->PlayEditorSound(CompileSuccessSound);
		return;
	}

	FString HyperlinkTarget = TEXT("file://") / FPaths::ConvertRelativePathToFull(Current->Task->Settings.OutputDirectory.Path / Current->Task->Settings.DocumentationTitle / TEXT("index.html"));
	auto OnHyperlinkClicked = [HyperlinkTarget]
	{
		UE_LOG(LogKantanDocGen, Log, TEXT("Invoking hyperlink"));
		FPlatformProcess::LaunchURL(*HyperlinkTarget, nullptr, nullptr);
	};

	auto HyperlinkText = TAttribute< FText >::Create(TAttribute< FText >::FGetter::CreateLambda([] { return LOCTEXT("GeneratedDocsHyperlink", "View docs"); }));
	// @TODO: Bug in SNotificationItemImpl::SetHyperlink, ignores non-delegate attributes... LOCTEXT("GeneratedDocsHyperlink", "View docs");

	Current->Task->Notification->SetText(LOCTEXT("DocConversionSuccessful", "Doc gen completed"));
	Current->Task->Notification->SetCompletionState(SNotificationItem::CS_Success);
	Current->Task->Notification->SetHyperlink(
		FSimpleDelegate::CreateLambda(OnHyperlinkClicked),
		HyperlinkText
	);
	Current->Task->Notification->ExpireAndFadeout();

	Current.Reset();
}

FDocGenTaskProcessor::EIntermediateProcessingResult FDocGenTaskProcessor::ProcessIntermediateDocs(FString const& IntermediateDir, FString const& OutputDir, FString const& DocTitle, bool bCleanOutput)
{
	auto& PluginManager = IPluginManager::Get();
	auto Plugin = PluginManager.FindPlugin(TEXT("KantanDocGen"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogKantanDocGen, Error, TEXT("Failed to locate plugin info"));
		return EIntermediateProcessingResult::UnknownError;
	}

	const FString DocGenToolBinPath = Plugin->GetBaseDir() / TEXT("ThirdParty") / TEXT("KantanDocGenTool") / TEXT("bin");
	const FString DocGenToolExeName = TEXT("KantanDocGen.exe");
	const FString DocGenToolPath = DocGenToolBinPath / DocGenToolExeName;

	// Create a read and write pipe for the child process
	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));

	FString Args =
		FString(TEXT("-outputdir=")) + TEXT("\"") + OutputDir + TEXT("\"")
		+ TEXT(" -fromintermediate -intermediatedir=") + TEXT("\"") + IntermediateDir + TEXT("\"")
		+ TEXT(" -name=") + DocTitle
		+ (bCleanOutput ? TEXT(" -cleanoutput") : TEXT(""))
		;
	UE_LOG(LogKantanDocGen, Log, TEXT("Invoking conversion tool: %s %s"), *DocGenToolPath, *Args);
	FProcHandle Proc = FPlatformProcess::CreateProc(
		*DocGenToolPath,
		*Args,
		true,
		false,
		false,
		nullptr,
		0,
		nullptr,
		PipeWrite
	);

	int32 ReturnCode = 0;
	if (Proc.IsValid())
	{
		FString BufferedText;
		for (bool bProcessFinished = false; !bProcessFinished; )
		{
			bProcessFinished = FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);

			/*			if(!bProcessFinished && Warn->ReceivedUserCancel())
			{
			FPlatformProcess::TerminateProc(ProcessHandle);
			bProcessFinished = true;
			}
			*/
			BufferedText += FPlatformProcess::ReadPipe(PipeRead);

			int32 EndOfLineIdx;
			while (BufferedText.FindChar('\n', EndOfLineIdx))
			{
				FString Line = BufferedText.Left(EndOfLineIdx);
				Line.RemoveFromEnd(TEXT("\r"));

				UE_LOG(LogKantanDocGen, Log, TEXT("[KantanDocGen] %s"), *Line);

				BufferedText = BufferedText.Mid(EndOfLineIdx + 1);
			}

			FPlatformProcess::Sleep(0.1f);
		}

		//FPlatformProcess::WaitForProc(Proc);
		//FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
		FPlatformProcess::CloseProc(Proc);
		Proc.Reset();

		if (ReturnCode != 0)
		{
			UE_LOG(LogKantanDocGen, Error, TEXT("KantanDocGen tool failed (code %i), see above output."), ReturnCode);
		}
	}

	// Close the pipes
	FPlatformProcess::ClosePipe(0, PipeRead);
	FPlatformProcess::ClosePipe(0, PipeWrite);

	switch (ReturnCode)
	{
	case 0:
		return EIntermediateProcessingResult::Success;
	case -1:
		return EIntermediateProcessingResult::UnknownError;
	case -2:
		return EIntermediateProcessingResult::DiskWriteFailure;
	default:
		return EIntermediateProcessingResult::SuccessWithErrors;
	}
}


#undef LOCTEXT_NAMESPACE
