#pragma once

// A scaled map of how the desktop is currently arranged, mirroring the
// "Displays" card in the macOS app's Devices overview.
//
// Reads QGuiApplication::screens() rather than the VDD enumeration, so it works
// in receiver-only builds too and stays correct for physical monitors. Repaints
// itself when Windows adds, removes or moves a screen — attaching a virtual
// display shows up here without any prompting from the sender code.

#include <QWidget>
#include <QColor>

class DisplayArrangement : public QWidget {
    Q_OBJECT
public:
    explicit DisplayArrangement(QWidget* parent = nullptr);

    // Highlight the displays currently being streamed, by device name
    // (e.g. "\\\\.\\DISPLAY3"). Qt reports the same names on Windows.
    void setActiveDisplays(const QStringList& deviceNames);

    QSize sizeHint() const override { return QSize(420, 200); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStringList m_active;
};
