#include "AssetPreviewSession.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRiderAssetPreview, Log, All);

namespace
{
    FString Encode(const TSharedRef<FJsonObject>& Object)
    {
        FString Text;
        FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
        return Text;
    }

    bool SendJson(const FHttpResultCallback& Complete, const TSharedRef<FJsonObject>& Object,
        EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
    {
        TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Encode(Object), TEXT("application/json; charset=utf-8"));
        Response->Code = Code;
        Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
        Complete(MoveTemp(Response));
        return true;
    }

    bool SendError(const FHttpResultCallback& Complete, const FString& Error,
        EHttpServerResponseCodes Code = EHttpServerResponseCodes::BadRequest)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("error"), Error);
        return SendJson(Complete, Object, Code);
    }
}

class FRiderAssetPreviewModule final : public IModuleInterface
{
    TSharedPtr<IHttpRouter> Router;
    TArray<FHttpRouteHandle> Routes;
    TMap<FString, TSharedPtr<FAssetPreviewSession>> Sessions;
    FString Token;
    FString DescriptorPath;
    FTSTicker::FDelegateHandle CleanupTicker;

    void RemoveExpiredSessions()
    {
        const double Now = FPlatformTime::Seconds();
        for (auto It = Sessions.CreateIterator(); It; ++It)
            if (Now - It.Value()->LastAccess > 1800) It.RemoveCurrent();
    }

    bool Handle(const FString& Route, const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        check(IsInGameThread()); // HTTPServer dispatches synchronously from its game-thread ticker.
        const TArray<FString>* Auth = Request.Headers.Find(TEXT("authorization"));
        if (!Request.PeerAddress.IsValid() || Request.PeerAddress->ToString(false) != TEXT("127.0.0.1")
            || Request.Headers.Contains(TEXT("origin")) || !Auth || Auth->Num() != 1
            || (*Auth)[0] != TEXT("Bearer ") + Token)
            return SendError(Complete, TEXT("Unauthorized preview request."), EHttpServerResponseCodes::Denied);
        if (Request.Body.IsEmpty() || Request.Body.Num() > 8192)
            return SendError(Complete, TEXT("Expected a JSON request body no larger than 8 KB."));
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
        const FString Body(Converted.Length(), Converted.Get());
        TSharedPtr<FJsonObject> Input;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Input) || !Input)
            return SendError(Complete, TEXT("Invalid JSON request."));

        // Recover preview worlds abandoned by a crashed/disconnected IDE.
        RemoveExpiredSessions();

        if (Route == TEXT("open") || Route == TEXT("snapshot"))
        {
            FString Filename;
            if (!Input->TryGetStringField(TEXT("filename"), Filename))
                return SendError(Complete, TEXT("Missing asset filename."));
            if (Sessions.Num() >= 8)
                return SendError(Complete, TEXT("Eight previews are already open. Close another preview tab or restart the bridge."));
            TSharedPtr<FAssetPreviewSession> Preview = MakeShared<FAssetPreviewSession>();
            FString Error;
            FString Snapshot;
            if (Route == TEXT("snapshot") && !Input->TryGetStringField(TEXT("snapshot"), Snapshot)) return SendError(Complete, TEXT("Missing revision snapshot."));
            if (Route == TEXT("snapshot"))
            {
                FString RequestId;
                FGuid ParsedId;
                if (!Input->TryGetStringField(TEXT("request"), RequestId) || !FGuid::ParseExact(RequestId, EGuidFormats::Digits, ParsedId)
                    || Sessions.Contains(RequestId)) return SendError(Complete, TEXT("Expected a unique revision request ID."));
                Preview->Id = RequestId; // The client can close a timed-out open without knowing its response.
            }
            if (!Preview->Open(Filename, Error, Snapshot)) return SendError(Complete, Error);
            TSharedRef<FJsonObject> Description = Route == TEXT("snapshot") ? Preview->Inspect() : Preview->Describe();
            if (FTCHARToUTF8(*Encode(Description)).Length() > 16 * 1024 * 1024)
                return SendError(Complete, TEXT("This asset exceeds the inspection response limit. Inspect a smaller asset or use Unreal's full diff tools."));
            Sessions.Add(Preview->Id, Preview);
            return SendJson(Complete, Description);
        }

        FString SessionId;
        if (!Input->TryGetStringField(TEXT("session"), SessionId)) return SendError(Complete, TEXT("Missing session ID."));
        if (Route == TEXT("close"))
        {
            Sessions.Remove(SessionId);
            return SendJson(Complete, MakeShared<FJsonObject>());
        }
        if (!Sessions.Contains(SessionId))
            return SendError(Complete, TEXT("Preview session expired. Use Reconnect / reload asset."), EHttpServerResponseCodes::NotFound);
        TSharedPtr<FAssetPreviewSession> Preview = Sessions[SessionId];
        Preview->Touch();
        if (Route == TEXT("details"))
        {
            FString Component;
            if (!Input->TryGetStringField(TEXT("component"), Component) || !Preview->HasComponent(Component))
                return SendError(Complete, TEXT("Unknown component."));
            bool bAdvanced = false;
            Input->TryGetBoolField(TEXT("advanced"), bAdvanced);
            return SendJson(Complete, Preview->Details(Component, bAdvanced));
        }
        if (Route == TEXT("frame"))
        {
            double Yaw, Pitch, Zoom;
            FString Focus;
            if (!Input->TryGetNumberField(TEXT("yaw"), Yaw) || !Input->TryGetNumberField(TEXT("pitch"), Pitch)
                || !Input->TryGetNumberField(TEXT("zoom"), Zoom) || !Input->TryGetStringField(TEXT("focus"), Focus)
                || !FMath::IsFinite(Yaw) || !FMath::IsFinite(Pitch) || !FMath::IsFinite(Zoom)
                || !Preview->HasComponent(Focus)) return SendError(Complete, TEXT("Invalid camera or component."));
            TArray<uint8> Png;
            FString Error;
            FBox SharedBounds(ForceInit);
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Input->TryGetArrayField(TEXT("bounds"), Values))
            {
                if (Values->Num() != 6) return SendError(Complete, TEXT("Invalid comparison bounds."));
                double Coordinates[6];
                for (int32 Index = 0; Index < 6; ++Index)
                    if (!(*Values)[Index]->TryGetNumber(Coordinates[Index]) || !FMath::IsFinite(Coordinates[Index]) || FMath::Abs(Coordinates[Index]) > 1.e10)
                        return SendError(Complete, TEXT("Invalid comparison bounds."));
                SharedBounds = FBox(FVector(Coordinates[0], Coordinates[1], Coordinates[2]), FVector(Coordinates[3], Coordinates[4], Coordinates[5]));
                if (SharedBounds.Min.X > SharedBounds.Max.X || SharedBounds.Min.Y > SharedBounds.Max.Y || SharedBounds.Min.Z > SharedBounds.Max.Z)
                    return SendError(Complete, TEXT("Invalid comparison bounds order."));
            }
            if (!Preview->Render(Yaw, Pitch, Zoom, Focus, Png, Error, SharedBounds.IsValid ? &SharedBounds : nullptr)) return SendError(Complete, Error);
            TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(MoveTemp(Png), TEXT("image/png"));
            Response->Code = EHttpServerResponseCodes::Ok;
            Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
            Complete(MoveTemp(Response));
            return true;
        }
        return SendError(Complete, TEXT("Unknown preview route."), EHttpServerResponseCodes::NotFound);
    }

public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet() || !GIsEditor) return;
        uint32 Port = 32187;
        FParse::Value(FCommandLine::Get(), TEXT("RiderAssetPreviewPort="), Port);
        if (Port < 1024 || Port > 65535) { UE_LOG(LogRiderAssetPreview, Error, TEXT("Invalid preview port.")); return; }
        // Honor the host's HTTPServer configuration, but fail closed if it would expose this listener.
        FString BindAddress = TEXT("localhost");
        GConfig->GetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), BindAddress, GEngineIni);
        TArray<FString> Overrides;
        GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        for (FString Override : Overrides)
        {
            Override.ReplaceInline(TEXT("("), TEXT("")); Override.ReplaceInline(TEXT(")"), TEXT(""));
            uint32 OverridePort = 0;
            if (FParse::Value(*Override, TEXT("Port="), OverridePort) && OverridePort == Port)
            {
                FParse::Value(*Override, TEXT("BindAddress="), BindAddress); break;
            }
        }
        if (!BindAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) && BindAddress != TEXT("127.0.0.1"))
        {
            UE_LOG(LogRiderAssetPreview, Error, TEXT("Preview requires a loopback HTTPServer listener. Configured address: %s"), *BindAddress);
            return;
        }
        Token = FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        // Start first so GetHttpRouter(..., true) actually detects a port-binding failure.
        FHttpServerModule::Get().StartAllListeners();
        Router = FHttpServerModule::Get().GetHttpRouter(Port, true);
        if (!Router) { UE_LOG(LogRiderAssetPreview, Error, TEXT("Could not bind preview port %u."), Port); return; }
        for (const FString Route : {TEXT("open"), TEXT("snapshot"), TEXT("details"), TEXT("frame"), TEXT("close")})
        {
            FHttpRouteHandle Handle = Router->BindRoute(FHttpPath(TEXT("/rap/v1/") + Route), EHttpServerRequestVerbs::VERB_POST,
                FHttpRequestHandler::CreateLambda([this, Route](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
                {
                    return this->Handle(Route, Request, Complete);
                }));
            if (!Handle)
            {
                UE_LOG(LogRiderAssetPreview, Error, TEXT("Could not register asset preview routes."));
                for (const FHttpRouteHandle& Registered : Routes) Router->UnbindRoute(Registered);
                Routes.Empty(); Router.Reset(); return;
            }
            Routes.Add(Handle);
        }
        DescriptorPath = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/session.json");
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(DescriptorPath), true);
        TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
        Descriptor->SetNumberField(TEXT("protocol"), 1);
        Descriptor->SetStringField(TEXT("endpoint"), FString::Printf(TEXT("http://127.0.0.1:%u"), Port));
        Descriptor->SetStringField(TEXT("token"), Token);
        Descriptor->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        Descriptor->SetNumberField(TEXT("inspectionVersion"), 2);
        if (!FFileHelper::SaveStringToFile(Encode(Descriptor), *DescriptorPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogRiderAssetPreview, Error, TEXT("Could not write preview session descriptor."));
            for (const FHttpRouteHandle& Route : Routes) Router->UnbindRoute(Route);
            Routes.Empty(); Router.Reset(); return;
        }
        UE_LOG(LogRiderAssetPreview, Display, TEXT("Asset preview bridge ready on loopback port %u. Session: %s"), Port, *DescriptorPath);
        CleanupTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
        {
            RemoveExpiredSessions(); return true;
        }), 60.f);
    }

    virtual void ShutdownModule() override
    {
        if (CleanupTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(CleanupTicker);
        if (Router)
            for (const FHttpRouteHandle& Route : Routes) Router->UnbindRoute(Route);
        Routes.Empty(); Sessions.Empty(); Router.Reset();
        // Do not stop listeners belonging to other editor plugins.
        if (!DescriptorPath.IsEmpty()) IFileManager::Get().Delete(*DescriptorPath);
    }
};

IMPLEMENT_MODULE(FRiderAssetPreviewModule, RiderAssetPreview)
