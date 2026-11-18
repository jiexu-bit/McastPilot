#include <QApplication>
#include <QFont>
#include <QStyleFactory>

#include "gui/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("McastPilot");
    QApplication::setApplicationVersion("0.3.0-m3");
    QApplication::setOrganizationName("McastPilot");

    app.setStyle(QStyleFactory::create("Fusion"));
    QFont font("Microsoft YaHei UI", 9);
    app.setFont(font);
    app.setStyleSheet(R"(
        QWidget {
            background: #0f1724;
            color: #e7edf7;
        }
        QFrame#card {
            background: #151d2c;
            border: 1px solid #243149;
            border-radius: 10px;
        }
        QLabel#cardTitle { color: #8d9bb3; font-size: 11px; }
        QLabel#cardValue { font-size: 20px; font-weight: 600; }
        QLabel#status { color: #8d9bb3; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QListWidget {
            background: #111826;
            border: 1px solid #243149;
            border-radius: 6px;
            padding: 5px 7px;
            selection-background-color: #3b82f6;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,
        QPlainTextEdit:focus {
            border-color: #3b82f6;
        }
        QPushButton {
            background: #1a2436;
            border: 1px solid #2b3a55;
            border-radius: 7px;
            padding: 6px 13px;
        }
        QPushButton:hover { background: #22304a; }
        QPushButton:pressed { background: #2b3a55; }
        QPushButton:disabled { color: #66738a; background: #141c2a; }
        QTableWidget {
            background: #111826;
            alternate-background-color: #141d2d;
            gridline-color: #243149;
            border: 1px solid #243149;
            border-radius: 10px;
        }
        QHeaderView::section {
            background: #151d2c;
            color: #8d9bb3;
            border: 0;
            border-bottom: 1px solid #243149;
            padding: 7px;
        }
        QTableWidget::item { padding: 6px; }
        QTableWidget::item:selected { background: #1e3a5f; color: #ffffff; }
        QMessageBox, QDialog { background: #151d2c; }
        QScrollBar:vertical {
            background: #0f1724;
            width: 11px;
        }
        QScrollBar::handle:vertical {
            background: #2b3a55;
            border-radius: 5px;
            min-height: 24px;
        }
    )");

    MainWindow window;
    window.show();
    return app.exec();
}
