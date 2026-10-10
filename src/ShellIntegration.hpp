#pragma once

#include <string>

namespace zipdesk {

bool shellIntegrationInstalled(const std::wstring& exePath);
void installShellIntegration(const std::wstring& exePath);
void removeShellIntegration();

}  // namespace zipdesk
