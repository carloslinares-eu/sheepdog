#include "gui/main_window.hpp"

#include "core/version.hpp"

namespace sheepdog::gui {

QString window_title() {
    const std::string_view v = sheepdog::version();
    return QStringLiteral("Sheepdog ") +
           QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(window_title());
    resize(1024, 640);
}

} // namespace sheepdog::gui
