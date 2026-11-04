#pragma once

#include <QMainWindow>
#include <QString>

namespace sheepdog::gui {

// "Sheepdog <version>", shown in the title bar.
QString window_title();

// The (still empty) main window. Menus, docks and the file table follow in 0.1-T8.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace sheepdog::gui
