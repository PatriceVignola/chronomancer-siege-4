// Copyright (c) 2006-2017 Audiokinetic Inc. / All Rights Reserved

#include "AkComponentCallbackManager.h"
#include "AkAudioDevice.h"
#include "AkInclude.h"
#include "AkAudioClasses.h"
#include "Misc/ScopeLock.h"
#include "Async/Async.h"

struct FAkComponentCallbackManager_Constants
{
	/// Optimization policy
	enum class Optimize
	{
		MemoryUsage,
		Speed,

		Value = MemoryUsage, ///< Set to either MemoryUsage or Speed
	};

	/// The default number of expected simultaneously playing sounds on a specific GameObject
	enum { ReserveSize = 8, };
};

FAkComponentCallbackManager* FAkComponentCallbackManager::Instance = nullptr;

FAkComponentCallbackManager* FAkComponentCallbackManager::GetInstance()
{
	return Instance;
}

void FAkFunctionPtrEventCallbackPackage::HandleAction(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
	if (pfnUserCallback)
	{
		in_pCallbackInfo->pCookie = pUserCookie;
		pfnUserCallback(in_eType, in_pCallbackInfo);
		in_pCallbackInfo->pCookie = (void*)this;
	}
}

void FAkBlueprintDelegateEventCallbackPackage::HandleAction(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
	if (BlueprintCallback.IsBound())
	{
		UAkCallbackInfo* CachedAkCallbackInfo = AkCallbackTypeHelpers::GetBlueprintableCallbackInfo(in_eType, in_pCallbackInfo);
		EAkCallbackType BlueprintCallbackType = AkCallbackTypeHelpers::GetBlueprintCallbackTypeFromAkCallbackType(in_eType);
		auto CachedBlueprintCallback = BlueprintCallback;
		AsyncTask(ENamedThreads::GameThread, [CachedAkCallbackInfo, BlueprintCallbackType, CachedBlueprintCallback]()
		{
			CachedBlueprintCallback.ExecuteIfBound(BlueprintCallbackType, CachedAkCallbackInfo);
		});
	}
}

void FAkLatentActionEventCallbackPackage::HandleAction(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
	if (EndOfEventLatentAction)
	{
		EndOfEventLatentAction->EventFinished = true;
	}
}

void FAkComponentCallbackManager::AkComponentCallback(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
	auto pPackage = (IAkUserEventCallbackPackage*)in_pCallbackInfo->pCookie;

	if (Instance && pPackage)
	{
		const auto& gameObjID = in_pCallbackInfo->gameObjID;
		bool deletePackage = false;

		{
			FScopeLock Lock(&Instance->CriticalSection);
			auto pPackageSet = Instance->GameObjectToPackagesMap.Find(gameObjID);
			if (pPackageSet && in_eType == AK_EndOfEvent)
			{
				Instance->RemovePackageFromSet(pPackageSet, pPackage, gameObjID);
				deletePackage = true;
			}
		}

		if((pPackage->uUserFlags & in_eType) != 0)
		{
			pPackage->HandleAction(in_eType, in_pCallbackInfo);
		}

		if (deletePackage)
			delete pPackage;
	}

}

FAkComponentCallbackManager::FAkComponentCallbackManager()
{
	if (Instance != nullptr)
	{
		UE_LOG(LogInit, Error, TEXT("FAkComponentCallbackManager has already been instantiated."));
	}

	Instance = this;
}

FAkComponentCallbackManager::~FAkComponentCallbackManager()
{
	for (auto& Item : GameObjectToPackagesMap)
		for (auto pPackage : Item.Value)
			delete pPackage;

	Instance = nullptr;
}

IAkUserEventCallbackPackage* FAkComponentCallbackManager::CreateCallbackPackage(AkCallbackFunc in_cbFunc, void* in_Cookie, uint32 in_Flags, AkGameObjectID in_gameObjID)
{
	auto pPackage = new FAkFunctionPtrEventCallbackPackage(in_cbFunc, in_Cookie, in_Flags);
	if (pPackage)
	{
		FScopeLock Lock(&CriticalSection);
		GameObjectToPackagesMap.FindOrAdd(in_gameObjID).Add(pPackage);
	}

	return pPackage;
}

IAkUserEventCallbackPackage* FAkComponentCallbackManager::CreateCallbackPackage(FOnAkPostEventCallback BlueprintCallback, uint32 in_Flags, AkGameObjectID in_gameObjID)
{
	auto pPackage = new FAkBlueprintDelegateEventCallbackPackage(BlueprintCallback, in_Flags);
	if (pPackage)
	{
		FScopeLock Lock(&CriticalSection);
		GameObjectToPackagesMap.FindOrAdd(in_gameObjID).Add(pPackage);
	}

	return pPackage;
}

IAkUserEventCallbackPackage* FAkComponentCallbackManager::CreateCallbackPackage(FWaitEndOfEventAction* LatentAction, AkGameObjectID in_gameObjID)
{
	auto pPackage = new FAkLatentActionEventCallbackPackage(LatentAction);
	if (pPackage)
	{
		FScopeLock Lock(&CriticalSection);
		GameObjectToPackagesMap.FindOrAdd(in_gameObjID).Add(pPackage);
	}

	return pPackage;
}

void FAkComponentCallbackManager::RemoveCallbackPackage(IAkUserEventCallbackPackage* in_Package, AkGameObjectID in_gameObjID)
{
	{
		FScopeLock Lock(&CriticalSection);
		auto pPackageSet = GameObjectToPackagesMap.Find(in_gameObjID);
		if (pPackageSet)
			RemovePackageFromSet(pPackageSet, in_Package, in_gameObjID);
	}

	delete in_Package;
}

void FAkComponentCallbackManager::RegisterGameObject(AkGameObjectID in_gameObjID)
{
	if (FAkComponentCallbackManager_Constants::Optimize::Value == FAkComponentCallbackManager_Constants::Optimize::Speed)
	{
		FScopeLock Lock(&CriticalSection);
		GameObjectToPackagesMap.FindOrAdd(in_gameObjID).Reserve(FAkComponentCallbackManager_Constants::ReserveSize);
	}
}

void FAkComponentCallbackManager::UnregisterGameObject(AkGameObjectID in_gameObjID)
{
	// after the following function call, all callbacks associated with this gameObjID will be completed
	AK::SoundEngine::CancelEventCallbackGameObject(in_gameObjID);

	FScopeLock Lock(&CriticalSection);
	auto pPackageSet = GameObjectToPackagesMap.Find(in_gameObjID);
	if (pPackageSet)
	{
		for (auto pPackage : *pPackageSet)
			delete pPackage;

		GameObjectToPackagesMap.Remove(in_gameObjID);
	}
}

bool FAkComponentCallbackManager::HasActiveEvents(AkGameObjectID in_gameObjID)
{
	FScopeLock Lock(&CriticalSection);
	auto pPackageSet = GameObjectToPackagesMap.Find(in_gameObjID);
	return pPackageSet && pPackageSet->Num() > 0;
}

void FAkComponentCallbackManager::RemovePackageFromSet(FAkComponentCallbackManager::PackageSet* in_pPackageSet, IAkUserEventCallbackPackage* in_pPackage, AkGameObjectID in_gameObjID)
{
	in_pPackageSet->Remove(in_pPackage);
	if (FAkComponentCallbackManager_Constants::Optimize::Value == FAkComponentCallbackManager_Constants::Optimize::MemoryUsage)
	{
		if (in_pPackageSet->Num() == 0)
			GameObjectToPackagesMap.Remove(in_gameObjID);
	}
}
