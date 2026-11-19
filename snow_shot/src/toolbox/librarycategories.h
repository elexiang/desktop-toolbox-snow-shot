// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCoreApplication>
#include <QSignalBlocker>
#include <QTabBar>

namespace toolbox {
inline QString currentCategory(const QTabBar* tabs) {
    return tabs->tabData(tabs->currentIndex()).toString();
}

inline void updateCategoryTabs(QTabBar* tabs, const QStringList& categories) {
    QStringList displayed;
    for (int i = 1; i < tabs->count(); ++i)
        displayed.append(tabs->tabData(i).toString());
    if (tabs->count() && displayed == categories)
        return;
    const auto current = currentCategory(tabs);
    const QSignalBlocker blocker(tabs);
    while (tabs->count())
        tabs->removeTab(tabs->count() - 1);
    tabs->addTab(QCoreApplication::translate("ToolboxLibrary", "All categories"));
    for (const auto& category : categories) {
        const int index = tabs->addTab(category.left(40));
        tabs->setTabData(index, category);
        tabs->setTabToolTip(index, category);
    }
    tabs->setCurrentIndex(categories.indexOf(current) + 1);
}
} // namespace toolbox
