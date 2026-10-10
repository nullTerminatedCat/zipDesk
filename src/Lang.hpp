#pragma once

#include <QString>

namespace zipdesk {

enum class UiLanguage { Chinese, English };

UiLanguage uiLanguage();
void setUiLanguage(UiLanguage language);
QString translate(const QString& chinese);

}  // namespace zipdesk
