#include "core/version.hpp"
#include "gui/main_window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Sheepdog"));
    QApplication::setOrganizationName(QStringLiteral("Sheepdog"));
    const std::string_view v = sheepdog::version();
    QApplication::setApplicationVersion(
        QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size())));

    sheepdog::gui::MainWindow window;
    window.show();
    return QApplication::exec();
}
