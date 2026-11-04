#include "core/version.hpp"
#include "gui/main_window.hpp"

#include <QApplication>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("the empty main window shows the version in its title", "[gui]") {
    int argc = 1;
    char name[] = "sheepdog_gui_tests";
    char* argv[] = {name, nullptr};
    QApplication app(argc, argv);

    sheepdog::gui::MainWindow window;
    window.show();
    QApplication::processEvents();

    const std::string title = window.windowTitle().toStdString();
    CHECK(title == "Sheepdog " + std::string(sheepdog::version()));
    CHECK(window.isVisible());
}
