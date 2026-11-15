#pragma once

#include "dllmain.h"
#include <vector>

namespace Hooks_Package {
    void Install();
    void Uninstall();

    void NotifyLicenseChanged();

    void RequestRequeryOnce();

    AccountID_t GetFamilySharedOwner(AppId_t appId);
    std::vector<AppId_t> GetKnownAppIds();

}
