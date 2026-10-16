#include "ProgressDialog.hpp"
#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <limits>
#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ProgressDialog dialog;
    auto* percent = dialog.findChild<QLabel*>("progressPercent");
    auto* bar = dialog.findChild<QProgressBar*>();
    auto* cancel = dialog.findChild<QPushButton*>();
    auto check = [](bool condition, const char* message) {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    };
    if (!check(percent && bar && cancel, "missing progress controls")) return 1;
    if (!check(bar->maximum() == 0 && !percent->text().contains('%'), "unknown initial total shown as percentage")) return 1;
    dialog.setTotal(100);
    dialog.setProcessed(50);
    if (!check(bar->value() == 500 && percent->text() == "50%", "normal progress")) return 1;
    dialog.setTotal(0);
    dialog.setProcessed(99);
    if (!check(bar->maximum() == 0 && !percent->text().contains('%'), "stale percentage after unknown total")) return 1;
    const quint64 maximum = std::numeric_limits<quint64>::max();
    dialog.setTotal(maximum);
    dialog.setProcessed(maximum / 2);
    if (!check(bar->value() >= 499 && bar->value() <= 500, "large counter overflow")) return 1;
    dialog.setProcessed(maximum);
    if (!check(bar->value() == 1000, "large counter completion")) return 1;
    dialog.setTotal(1);
    dialog.setProcessed(maximum);
    if (!check(bar->value() == 1000, "overrun not clamped")) return 1;
    QSignalSpy requests(&dialog, &ProgressDialog::cancelRequested);
    cancel->click();
    const QString cancelling = percent->text();
    dialog.close();
    dialog.setTotal(100);
    dialog.setProcessed(25);
    if (!check(requests.count() == 1 && !cancel->isEnabled() && percent->text() == cancelling,
               "duplicate cancellation or late callback changed state")) return 1;
    dialog.reset("Next task");
    if (!check(cancel->isEnabled() && bar->maximum() == 0 && !percent->text().contains('%'), "reset retained state")) return 1;
    cancel->click();
    if (!check(requests.count() == 2, "next task cannot cancel")) return 1;
    std::cout << "PROGRESS UI OK\n";
    return 0;
}
