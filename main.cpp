#include <QApplication> // Import the core Qt Application management class
#include "mainwindow.h" // Import the MainWindow class definition containing our GUI layout and logic

// The main entrypoint function where execution of the program begins
int main(int argc, char *argv[])
{
    // Initialize the Qt Application framework. This handles window management,
    // mouse clicks, keyboard presses, screen scaling, and desktop settings.
    QApplication app(argc, argv);

    // Apply the "Fusion" theme style. This is a built-in clean, modern, and consistent
    // user interface style provided by Qt that looks identical across Windows, Linux, and macOS.
    app.setStyle("Fusion");

    // Create the Main Window object (this instantiates the layout, sidebar controls,
    // dashboard cards, data tables, and database connections).
    MainWindow w;
    
    // Display the Main Window on the screen.
    w.show();

    // Start the application event loop (exec). This keeps the program running,
    // processing button clicks and system ticks until the user closes the window.
    return app.exec();
}
