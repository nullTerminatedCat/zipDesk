#pragma once

#include "ArchiveService.hpp"

#include <string>
#include <vector>

namespace zipdesk {

bool isCliInvocation(const std::vector<std::wstring>& args);
int runCli(ArchiveService& service, const std::vector<std::wstring>& args);

}  // namespace zipdesk
