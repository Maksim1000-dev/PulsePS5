#include "launcherTheme.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QObject>
#include <QStyleFactory>
#include <QTimer>

namespace {

constexpr auto PS5_STYLE = R"QSS(
/* PulsePS5 launcher: PlayStation 5 inspired dark interface */
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10pt;
    outline: none;
}

QWidget {
    color: #f4f7ff;
    background-color: #101522;
    selection-background-color: #246bdb;
    selection-color: #ffffff;
}

QDialog, QMainWindow {
    background-color: #0b1020;
}

QFrame#pulse_header {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                stop:0 #14233b, stop:1 #0e1728);
    border: 1px solid #2c4264;
    border-radius: 16px;
}

QFrame#pulse_hero_panel {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                stop:0 #16233a, stop:0.65 #111b2d, stop:1 #0e1728);
    border: 1px solid #293d5d;
    border-radius: 16px;
}

QLabel#hero_art {
    color: #b6d6ff;
    background-color: #1d3150;
    border: 1px solid #3c5a82;
    border-radius: 12px;
    font-size: 24pt;
    font-weight: 700;
}

QLabel#hero_eyebrow {
    color: #79b4ff;
    font-size: 8pt;
    font-weight: 700;
}

QLabel#hero_title {
    color: #f7faff;
    font-size: 23pt;
    font-weight: 700;
}

QLabel#hero_metadata {
    color: #aab8cf;
    font-size: 10pt;
}

QPushButton#hero_play_button {
    color: #ffffff;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #3588ff, stop:1 #1460cf);
    border: 1px solid #77afff;
    border-radius: 9px;
    font-weight: 700;
    letter-spacing: 1px;
    min-height: 34px;
    padding: 2px 24px;
}

QPushButton#hero_play_button:hover {
    background: #4594ff;
}

QLabel#pulse_brand_label {
    color: #f7faff;
    font-size: 24pt;
    font-weight: 700;
}

QLabel#pulse_tagline_label {
    color: #85b7ff;
    font-size: 8pt;
    font-weight: 600;
}

QLabel#pulse_library_badge {
    color: #d7e8ff;
    background-color: #1b3150;
    border: 1px solid #365783;
    border-radius: 12px;
    font-size: 8pt;
    font-weight: 700;
    padding: 8px 14px;
}

QLabel {
    background: transparent;
    color: #e8edfa;
}

QLabel:disabled {
    color: #65718a;
}

QPushButton, QToolButton {
    color: #f7f9ff;
    background-color: #202b42;
    border: 1px solid #344563;
    border-radius: 9px;
    padding: 8px 16px;
    min-height: 20px;
}

QPushButton:hover, QToolButton:hover {
    background-color: #2b3b59;
    border-color: #4c8dff;
}

QPushButton:pressed, QToolButton:pressed {
    background-color: #174da7;
    border-color: #66a0ff;
}

QPushButton:disabled, QToolButton:disabled {
    color: #68758d;
    background-color: #171e2d;
    border-color: #252f43;
}

QPushButton:default {
    background-color: #1769e0;
    border: 1px solid #4b91ff;
}

QPushButton:default:hover {
    background-color: #287cf4;
}

QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox,
QComboBox {
    color: #f4f7ff;
    background-color: #151e30;
    border: 1px solid #34445f;
    border-radius: 8px;
    padding: 7px 10px;
    selection-background-color: #246bdb;
    min-height: 20px;
}

QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border: 1px solid #4c91ff;
    background-color: #18243a;
}

QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {
    color: #68758d;
    background-color: #111827;
}

QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 28px;
    border: none;
    border-left: 1px solid #34445f;
    border-top-right-radius: 8px;
    border-bottom-right-radius: 8px;
}

QComboBox::down-arrow {
    width: 8px;
    height: 8px;
}

QComboBox QAbstractItemView {
    color: #f4f7ff;
    background-color: #18243a;
    border: 1px solid #3b5275;
    selection-background-color: #205fc3;
    selection-color: #ffffff;
    padding: 5px;
}

QCheckBox, QRadioButton {
    spacing: 9px;
    color: #e5ebf8;
    background: transparent;
}

QCheckBox::indicator, QRadioButton::indicator {
    width: 17px;
    height: 17px;
    border: 1px solid #60718f;
    background-color: #151e30;
}

QCheckBox::indicator {
    border-radius: 5px;
}

QRadioButton::indicator {
    border-radius: 9px;
}

QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background-color: #2878ed;
    border: 1px solid #73aaff;
}

QGroupBox {
    color: #f4f7ff;
    background-color: rgba(24, 35, 56, 0.72);
    border: 1px solid #2b3b57;
    border-radius: 12px;
    margin-top: 13px;
    padding: 15px 10px 10px 10px;
    font-weight: 600;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 13px;
    padding: 0 7px;
    color: #8dbaff;
    background-color: #101827;
}

QTreeView, QTreeWidget, QListView, QListWidget, QTableView, QTableWidget {
    color: #eaf0ff;
    background-color: #111a2a;
    alternate-background-color: #162238;
    border: 1px solid #2d3e5b;
    border-radius: 10px;
    gridline-color: #263650;
    padding: 5px;
}

QTreeView::item, QTreeWidget::item, QListView::item, QListWidget::item,
QTableView::item, QTableWidget::item {
    padding: 6px;
    border-radius: 5px;
}

QTreeView::item:hover, QTreeWidget::item:hover, QListView::item:hover,
QListWidget::item:hover, QTableView::item:hover, QTableWidget::item:hover {
    background-color: #1d3150;
}

QTreeView::item:selected, QTreeWidget::item:selected, QListView::item:selected,
QListWidget::item:selected, QTableView::item:selected, QTableWidget::item:selected {
    color: #ffffff;
    background-color: #205fc3;
}

QHeaderView::section {
    color: #a9c9ff;
    background-color: #1a2941;
    border: none;
    border-bottom: 1px solid #3a5277;
    padding: 8px 7px;
    font-weight: 600;
}

QTabWidget::pane {
    border: 1px solid #2d3e5b;
    border-radius: 9px;
    background-color: #111a2a;
    top: -1px;
}

QTabBar::tab {
    color: #aebbd2;
    background-color: #151f31;
    border: 1px solid #293a56;
    border-bottom: none;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
    padding: 9px 16px;
    margin-right: 4px;
}

QTabBar::tab:selected {
    color: #ffffff;
    background-color: #203b63;
    border-color: #4b83cf;
}

QTabBar::tab:hover:!selected {
    background-color: #1b2b45;
}

QProgressBar {
    color: #ffffff;
    background-color: #172238;
    border: 1px solid #344967;
    border-radius: 6px;
    text-align: center;
    min-height: 13px;
}

QProgressBar::chunk {
    background-color: #2878ed;
    border-radius: 5px;
}

QSlider::groove:horizontal {
    height: 6px;
    background: #2b3b56;
    border-radius: 3px;
}

QSlider::sub-page:horizontal {
    background: #2878ed;
    border-radius: 3px;
}

QSlider::handle:horizontal {
    background: #f4f8ff;
    border: 2px solid #4a91ff;
    width: 15px;
    height: 15px;
    margin: -6px 0;
    border-radius: 8px;
}

QScrollBar:vertical {
    background: #101827;
    width: 12px;
    margin: 3px;
    border-radius: 6px;
}

QScrollBar::handle:vertical {
    background: #354968;
    min-height: 28px;
    border-radius: 5px;
}

QScrollBar::handle:vertical:hover {
    background: #4b75ad;
}

QScrollBar:horizontal {
    background: #101827;
    height: 12px;
    margin: 3px;
    border-radius: 6px;
}

QScrollBar::handle:horizontal {
    background: #354968;
    min-width: 28px;
    border-radius: 5px;
}

QScrollBar::add-line, QScrollBar::sub-line {
    width: 0px;
    height: 0px;
    background: transparent;
}

QMenu {
    color: #edf3ff;
    background-color: #18243a;
    border: 1px solid #3b5275;
    border-radius: 8px;
    padding: 5px;
}

QMenu::item {
    padding: 7px 28px 7px 12px;
    border-radius: 5px;
}

QMenu::item:selected {
    background-color: #205fc3;
}

QMenu::separator {
    height: 1px;
    background: #344967;
    margin: 5px 8px;
}

QToolTip {
    color: #ffffff;
    background-color: #1b2a43;
    border: 1px solid #4b83cf;
    padding: 6px 8px;
    border-radius: 6px;
}

QDialogButtonBox {
    button-layout: 0;
}

QFrame[frameShape="4"], QFrame[frameShape="5"] {
    color: #2d3e5b;
}
)QSS";

class PaletteChangeFilter final : public QObject {
public:
    explicit PaletteChangeFilter(QApplication& application)
        : QObject(&application), m_application(application) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::ApplicationPaletteChange && !m_refresh_pending) {
            m_refresh_pending = true;
            QTimer::singleShot(0, &m_application, [this] {
                m_application.setStyleSheet(QString::fromUtf8(PS5_STYLE));
                m_refresh_pending = false;
            });
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QApplication& m_application;
    bool m_refresh_pending = false;
};

} // namespace

namespace LauncherTheme {

void Initialize(QApplication& application) {
    // Fusion provides consistent widget rendering across Windows, Linux and macOS.
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
        application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    application.setFont(font);
    application.setStyleSheet(QString::fromUtf8(PS5_STYLE));
    application.installEventFilter(new PaletteChangeFilter(application));
}

} // namespace LauncherTheme
