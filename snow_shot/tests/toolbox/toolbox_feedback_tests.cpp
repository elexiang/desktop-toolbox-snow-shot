// SPDX-License-Identifier: GPL-3.0-or-later
#include "appfeedback.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto require = [](bool value, const char* message) {
        if (!value) {
            std::fprintf(stderr, "%s\n", message);
            std::exit(1);
        }
    };
    QWidget host;
    host.show();
    host.activateWindow();
    app.processEvents();
    toolbox::AppFeedback::show(QStringLiteral("Settings"), QStringLiteral("Saved"));
    QPointer<QWidget> first = toolbox::AppFeedback::current();
    require(first && first->isVisible(), "feedback appears");
    require(first->testAttribute(Qt::WA_ShowWithoutActivating) &&
                first->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                first->focusPolicy() == Qt::NoFocus,
            "feedback cannot interrupt keyboard or pointer input");
    toolbox::AppFeedback::show(QStringLiteral("Second"));
    auto second = toolbox::AppFeedback::current();
    require(second && second != first && (!first || !first->isVisible()),
            "new feedback replaces previous toast");
    QTimer::singleShot(2900, &app, [&] {
        require(!toolbox::AppFeedback::current() || !toolbox::AppFeedback::current()->isVisible(),
                "feedback disappears automatically");
        app.quit();
    });
    return app.exec();
}
