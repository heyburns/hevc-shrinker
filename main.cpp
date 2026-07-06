#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Apply Fusion theme styling for a clean cross-platform dark look
    app.setStyle("Fusion");

    MainWindow w;
    w.show();

    return app.exec();
}
