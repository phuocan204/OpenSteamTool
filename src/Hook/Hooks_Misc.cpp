#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/HookSupport/VehCommon.h"
#include "dllmain.h"
#include <cctype>
#include <cstdlib>
#include <windows.h>

namespace {
    // ── Resolve-only functions ─────────────────────────────────────
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    // ── VEH-captured functions (one-shot int3) ───────────────────────────────
    // On int3 hit, ctx->Rcx is stored to the named output variable.
    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t,      g_steamEngine,    void*);
    CAPTURE_THIS_FUNC(GetAppDataFromAppInfo,  int64,        g_pCAppInfoCache, void*, AppId_t, const char*, uint8*, int32);

    AppId_t   g_OnlineFixRealAppId = 0;
    AppId_t   g_OnlineFixFakeAppId = kOnlineFixAppId;
    bool      g_NetworkingSocketsActive = false;
    bool      g_SuppressAppIdFlip = false;
    bool      g_OnlineFixGamesPlayedSeen = false;
    std::unordered_map<AppId_t, std::string> g_GameNameCache;

    static const char* FindFlagToken(const char* cmdLine, const char* flag) {
        if (!cmdLine || !flag) return nullptr;
        const size_t flagLen = strlen(flag);
        const char* cur = cmdLine;
        while ((cur = strstr(cur, flag)) != nullptr) {
            const bool validLeft = (cur == cmdLine) || std::isspace(static_cast<unsigned char>(cur[-1])) || cur[-1] == '"';
            const char next = cur[flagLen];
            const bool validRight = next == '\0' || std::isspace(static_cast<unsigned char>(next)) || next == '"' || next == '=' || next == ':';
            if (validLeft && validRight) return cur + flagLen;
            cur += flagLen;
        }
        return nullptr;
    }

    static AppId_t ParseFlagAppId(const char* cmdLine, const char* flag) {
        const char* after = FindFlagToken(cmdLine, flag);
        if (!after) return 0;
        if (*after == '=' || *after == ':') {
            ++after;
        } else {
            while (*after && std::isspace(static_cast<unsigned char>(*after))) ++after;
        }
        if (!std::isdigit(static_cast<unsigned char>(*after))) return 0;
        char* endPtr = nullptr;
        unsigned long parsed = std::strtoul(after, &endPtr, 10);
        if (endPtr == after || parsed == 0) return 0;
        return static_cast<AppId_t>(parsed);
    }

    static bool TryReadIniFakeAppId(const std::filesystem::path& dir, AppId_t& outFakeAppId) {
        if (dir.empty()) return false;
        std::error_code ec;
        std::filesystem::path iniPath = dir / "OnlineFix.ini";
        if (!std::filesystem::is_regular_file(iniPath, ec)) return false;

        std::string iniStr = iniPath.string();
        UINT val = GetPrivateProfileIntA("Main", "FakeAppId", 0, iniStr.c_str());
        if (val == 0) {
            val = GetPrivateProfileIntA("OnlineFix", "FakeAppId", 0, iniStr.c_str());
        }
        outFakeAppId = val != 0 ? static_cast<AppId_t>(val) : kOnlineFixAppId;
        return true;
    }

    static bool DetectOnlineFixFromIni(const char* workingDir, const char* exePath, AppId_t& outFakeAppId) {
        if (workingDir && workingDir[0] != '\0') {
            if (TryReadIniFakeAppId(std::filesystem::path(workingDir), outFakeAppId)) return true;
        }
        if (exePath && exePath[0] != '\0') {
            std::filesystem::path exeParent = std::filesystem::path(exePath).parent_path();
            if (TryReadIniFakeAppId(exeParent, outFakeAppId)) return true;
        }
        return false;
    }

    static void ResetOnlineFixState() {
        g_OnlineFixRealAppId = 0;
        g_OnlineFixFakeAppId = kOnlineFixAppId;
        g_NetworkingSocketsActive = false;
        g_SuppressAppIdFlip = false;
        g_OnlineFixGamesPlayedSeen = false;
    }

    // ── SpawnProcess interception ────────────────────────────────────────────
    // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
    //                    pGameID, ...)
    // arg1=pCUser, arg2=pExePath, arg3=pCommandLine, arg4=pWorkingDir
    // arg5=pGameID (CGameID*; low 24 bits = AppId)
    static void OnSpawnProcessHit(OSTPlatform::Trap::Context& ctx, const VehCommon::Int3Site& /*site*/) {
        const char* exePath = VehCommon::GetArg<const char*>(ctx, 2);
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);
        const char* workingDir = VehCommon::GetArg<const char*>(ctx, 4);
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));

        LOG_MISC_DEBUG("SpawnProcess hit: appid {} cmd=\"{}\"", appId, cmdLine ? cmdLine : "<null>");

        if (FindFlagToken(cmdLine, "-noonlinefix")) {
            if (appId == g_OnlineFixRealAppId || !g_OnlineFixGamesPlayedSeen) {
                ResetOnlineFixState();
            }
            return;
        }

        const bool hasFlag = FindFlagToken(cmdLine, "-onlinefix") != nullptr;
        AppId_t iniFakeAppId = kOnlineFixAppId;
        const bool hasIni = !hasFlag && appId != 0 && DetectOnlineFixFromIni(workingDir, exePath, iniFakeAppId);

        if (hasFlag || hasIni) {
            AppId_t fakeAppId = ParseFlagAppId(cmdLine, "-fakeappid");
            if (fakeAppId == 0 && hasFlag) fakeAppId = ParseFlagAppId(cmdLine, "-onlinefix");
            if (fakeAppId == 0) fakeAppId = hasIni ? iniFakeAppId : kOnlineFixAppId;

            g_OnlineFixRealAppId = appId;
            g_OnlineFixFakeAppId = fakeAppId;
            g_NetworkingSocketsActive = false;
            g_OnlineFixGamesPlayedSeen = false;
            g_SuppressAppIdFlip = FindFlagToken(cmdLine, "-realappid") != nullptr;
            pGameID->SetAppID(fakeAppId);
            LOG_MISC_INFO("SpawnProcess: appid {} -> {}, realappid={}, source={}, cmd=\"{}\"",
                          appId, fakeAppId, g_SuppressAppIdFlip, hasFlag ? "cmdline" : "ini", cmdLine ? cmdLine : "");
            return;
        }

        if (g_OnlineFixRealAppId != 0 && appId == g_OnlineFixRealAppId) {
            pGameID->SetAppID(g_OnlineFixFakeAppId);
            LOG_MISC_INFO("SpawnProcess: child/relaunch appid {} -> {}, cmd=\"{}\"",
                          appId, g_OnlineFixFakeAppId, cmdLine ? cmdLine : "");
            return;
        }

        if (appId != 0 && !g_OnlineFixGamesPlayedSeen) {
            ResetOnlineFixState();
        }
    }

    // ── SteamController_OptedInMask ──────────────────────────────────────────
    // Called by CUser_BuildSpawnEnvBlock with pGameID's appid to
    // compute EnableConfiguratorSupport and the SDL_* env vars.
    // With 480 the spawned game inherits Spacewar's Steam Input
    // opt-in and gameoverlayrenderer hijacks the XInput stream.
    HOOK_FUNC(OptedInMask, int64,void* pThis, AppId_t appId)
    {
        if (g_OnlineFixRealAppId && appId == g_OnlineFixFakeAppId) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}",appId, g_OnlineFixRealAppId);
            appId = g_OnlineFixRealAppId;
        }
        return oOptedInMask(pThis, appId);
    }

    // ── CUser_BuildSpawnEnvBlock ─────────────────────────────────────────────
    // pOverlayCGameID drives SteamOverlayGameId, which the in-game
    // overlay reads for screenshot tags, community URLs, and asset
    // selection.  pCGameID drives SteamGameId / SteamAppId; leave it
    // at 480 so the in-game ownership bypass holds.
    HOOK_FUNC(BuildSpawnEnvBlock, int64,
              void* pThis, CGameID* pCGameID, void* a3, void* env,
              CGameID* pOverlayCGameID, void* a6, int a7,
              void* a8, void* a9, unsigned int a10, char a11)
    {
        if (g_OnlineFixRealAppId && pOverlayCGameID
            && pOverlayCGameID->AppID(true) == g_OnlineFixFakeAppId)
        {
            LOG_MISC_INFO("BuildSpawnEnvBlock: SetAppID in OverlayCGameID {} -> {}",
                          pOverlayCGameID->AppID(true), g_OnlineFixRealAppId);
            pOverlayCGameID->SetAppID(g_OnlineFixRealAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                    pOverlayCGameID, a6, a7,
                                    a8, a9, a10, a11);
    }

    // CAppInfoCache::GetOrAddAppData
    // The injected package keeps Lua-provided ids in PackageInfo::AppIdVec.
    // Some of those ids can actually be depot ids, but we cannot trust the
    // Lua config to classify app ids and depot ids for us. In offline mode,
    // depot ids usually have only placeholder appinfo data. That blocks
    // CClientAppManager_ProcessPendingLicenseUpdates, because it waits for
    // every AppIdVec entry to have resolved appinfo unless the entry has been
    // marked as a known-unknown id by the PICS path. For injected ids that
    // still have placeholder appinfo, set skip_flag so Steam treats them like
    // PICS unknown_appids instead of keeping the license update pending.
    HOOK_FUNC(GetOrAddAppData,CAppData*,void* pCache, AppId_t appId,bool bCreate)
    {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bCreate);
        // LOG_MISC_TRACE("GetOrAddAppData: appId={} bCreate={} -> pData={}", appId, bCreate, pData ? pData->DebugString() : "null");
        // TODO: find a more robust way
        if (LuaConfig::HasDepot(appId, false) && pData && !bCreate && pData->IsUnresolvedAppInfo()) {
            LOG_MISC_DEBUG("GetOrAddAppData: Marking appId {} as skip_flag=true to bypass license update blocking", appId);
            pData->bSkipFlag = true;
        }
        return pData;
    }
}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);

        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_CAPTURE_C(GetAppDataFromAppInfo);

        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        // INSTALL_HOOK_C(GetOrAddAppData);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        // UNINSTALL_HOOK(GetOrAddAppData);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) {
            LOG_MISC_WARN("GetAppIDForCurrentPipeWrap called before capture — returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId={}", appid);
        }
        return appid;
    }

    
    AppId_t ResolveAppId() {
        if (g_OnlineFixRealAppId) return g_OnlineFixRealAppId;
        return GetAppIDForCurrentPipeWrap();
    }

    bool IsOnlineFixActive() {
        return g_OnlineFixRealAppId != 0;
    }

    AppId_t GetOnlineFixFakeAppId() {
        return g_OnlineFixFakeAppId;
    }

    bool IsOnlineFixFakeAppId(AppId_t appId) {
        return g_OnlineFixRealAppId != 0 && appId == g_OnlineFixFakeAppId;
    }

    void OnGamesPlayedUpdated(bool hasFakeAppIdRunning) {
        if (!g_OnlineFixRealAppId) return;
        if (hasFakeAppIdRunning) {
            g_OnlineFixGamesPlayedSeen = true;
        } else if (g_OnlineFixGamesPlayedSeen) {
            LOG_MISC_INFO("OnlineFix session ended for appid {}", g_OnlineFixRealAppId);
            ResetOnlineFixState();
        }
    }

    void NotifyNetworkingSocketsUsed() {
        if (g_OnlineFixRealAppId && !g_NetworkingSocketsActive) {
            g_NetworkingSocketsActive = true;
            LOG_MISC_INFO("NetworkingSockets active: GetAppID now reports {} (fake={})",
                          g_SuppressAppIdFlip ? "real appid (-realappid)" : "fake appid for cert match",
                          g_OnlineFixFakeAppId);
        }
    }

    bool ShouldReportOnlineFixAppId() {
        // 翻转的用途是让 P2P socket 的 appid 与 480 会话证书对上，部分游戏靠它联机。
        // 但它一旦触发，本进程余下生命周期里每次询问都答 480；另一些游戏在启动期
        // 问同一句话拿到 480 就会卡死或黑屏。两侧都有游戏需要，而这个调用本身
        // 分不出对方是哪一类，于是用 -realappid 逐局退出。
        if (g_SuppressAppIdFlip) return false;
        return g_OnlineFixRealAppId != 0 && g_NetworkingSocketsActive;
    }
    
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut)
    {
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            if(updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        LOG_MISC_WARN("EnsureBufferCapacity: oCUtlBufferEnsureCapacity not resolved");
        return false;
    }

    // ── Game name ────────────────────────────────────────────────
    std::string GetGameNameByAppID(AppId_t appId)
    {
        auto it = g_GameNameCache.find(appId);
        if (it != g_GameNameCache.end()) return it->second;

        std::string name;

        if (CAPTURE_READY(GetAppDataFromAppInfo)) {
            char buf[256] = {};
            // "common/name" triggers auto-localization: the function detects
            // prefix "common" (keyType=2) + key "name", then tries
            // "name_localized/<current_lang>" before falling back to "name".
            // Returns strlen+1 on success, -1 on failure.
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1)
                name.assign(buf, static_cast<size_t>(len - 1));
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, name);
        g_GameNameCache[appId] = name;
        return name;
    }

}
