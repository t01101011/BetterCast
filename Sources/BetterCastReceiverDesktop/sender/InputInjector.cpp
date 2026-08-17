#include "InputInjector.h"
#include "../InputEvent.h"
#include "../KeyCodeMap.h"
#include "../MainWindow.h"  // LogManager

#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <Windows.h>

#pragma comment(lib, "user32.lib")

InputInjector::InputInjector(QObject* parent)
    : QObject(parent)
{
}

void InputInjector::setTargetBounds(const QRect& bounds) {
    if (bounds.isValid() && !bounds.isEmpty()) {
        m_bounds = bounds;
        LogManager::instance().log(
            QString("Input: Target display bounds %1,%2 %3x%4")
                .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()));
    }
}

bool InputInjector::setTargetDisplayName(const QString& deviceName) {
    if (deviceName.isEmpty()) return false;

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(reinterpret_cast<LPCWSTR>(deviceName.utf16()),
                              ENUM_CURRENT_SETTINGS, &dm)) {
        LogManager::instance().log("Input: Could not resolve bounds for " + deviceName);
        return false;
    }

    setTargetBounds(QRect(dm.dmPosition.x, dm.dmPosition.y,
                          static_cast<int>(dm.dmPelsWidth),
                          static_cast<int>(dm.dmPelsHeight)));
    return true;
}

bool InputInjector::isDuplicate(quint64 eventId) {
    if (eventId == 0) return false;               // unset id — cannot dedupe
    if (m_recentIds.contains(eventId)) return true;

    m_recentIds.insert(eventId);
    m_recentQueue.enqueue(eventId);
    if (m_recentQueue.size() > kMaxRecentEvents) {
        m_recentIds.remove(m_recentQueue.dequeue());
    }
    return false;
}

// Map a normalised point on the streamed display to SendInput's absolute
// coordinate space: 0..65535 spanning the whole virtual desktop.
bool InputInjector::toAbsolute(double nx, double ny, long& ax, long& ay) const {
    if (!m_bounds.isValid() || m_bounds.isEmpty()) return false;

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return false;

    nx = qBound(0.0, nx, 1.0);
    ny = qBound(0.0, ny, 1.0);

    const double px = m_bounds.x() + nx * m_bounds.width();
    const double py = m_bounds.y() + ny * m_bounds.height();

    // MOUSEEVENTF_VIRTUALDESK normalises against the virtual desktop, whose
    // origin can be negative when a monitor sits left of or above the primary.
    ax = qRound((px - vx) * 65535.0 / (vw - 1));
    ay = qRound((py - vy) * 65535.0 / (vh - 1));
    return true;
}

void InputInjector::reportFailureOnce(const QString& reason) {
    if (m_reportedFailure) return;
    m_reportedFailure = true;
    LogManager::instance().log("Input: " + reason);
    emit injectionBlocked(reason);
}

void InputInjector::injectMouse(double nx, double ny, uint32_t buttonFlags) {
    long ax = 0, ay = 0;
    if (!toAbsolute(nx, ny, ax, ay)) return;

    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = ax;
    in.mi.dy = ay;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
                    | buttonFlags;

    if (SendInput(1, &in, sizeof(INPUT)) == 0) {
        reportFailureOnce(
            QString("SendInput refused (error %1). Input cannot reach elevated "
                    "windows unless BetterCast also runs as administrator.")
                .arg(GetLastError()));
    }
}

void InputInjector::injectScroll(double nx, double ny, double deltaX, double deltaY,
                                 uint16_t gestureMode) {
    long ax = 0, ay = 0;
    const bool havePos = toAbsolute(nx, ny, ax, ay);

    // Park the cursor first — Windows delivers wheel messages to whatever is
    // under the pointer, not to the focused window.
    if (havePos) injectMouse(nx, ny, 0);

    // gestureMode reuses InputEvent.keyCode, matching the macOS sender's
    // convention: 1 = pinch zoom, 2 = rotation, 3 = smart zoom, 0 = plain scroll.
    const bool zoomGesture = (gestureMode == 1 || gestureMode == 3);

    if (zoomGesture) {
        // Ctrl+wheel is the Windows zoom idiom (macOS uses ⌘+wheel).
        INPUT seq[3] = {};
        seq[0].type = INPUT_KEYBOARD;
        seq[0].ki.wVk = VK_CONTROL;

        seq[1].type = INPUT_MOUSE;
        seq[1].mi.dwFlags = MOUSEEVENTF_WHEEL;
        seq[1].mi.mouseData = static_cast<DWORD>(qRound(deltaY * WHEEL_DELTA / 10.0));

        seq[2].type = INPUT_KEYBOARD;
        seq[2].ki.wVk = VK_CONTROL;
        seq[2].ki.dwFlags = KEYEVENTF_KEYUP;

        if (SendInput(3, seq, sizeof(INPUT)) == 0) {
            reportFailureOnce(QString("SendInput refused (error %1)").arg(GetLastError()));
        }
        return;
    }

    INPUT in[2] = {};
    int count = 0;
    if (deltaY != 0.0) {
        in[count].type = INPUT_MOUSE;
        in[count].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[count].mi.mouseData = static_cast<DWORD>(qRound(deltaY * WHEEL_DELTA / 10.0));
        count++;
    }
    if (deltaX != 0.0) {
        in[count].type = INPUT_MOUSE;
        in[count].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in[count].mi.mouseData = static_cast<DWORD>(qRound(deltaX * WHEEL_DELTA / 10.0));
        count++;
    }
    if (count > 0 && SendInput(count, in, sizeof(INPUT)) == 0) {
        reportFailureOnce(QString("SendInput refused (error %1)").arg(GetLastError()));
    }
}

void InputInjector::injectKey(uint16_t macKeyCode, bool down) {
    const uint16_t vk = KeyCodeMap::macToVk(macKeyCode, m_commandAsControl);
    if (vk == 0) {
        LogManager::instance().log(
            QString("Input: Unmapped mac key code 0x%1").arg(macKeyCode, 0, 16));
        return;
    }

    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    // Supply the scan code too — some games and remote-desktop stacks read it
    // in preference to the virtual key.
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = 0;
    if (KeyCodeMap::isExtendedVk(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down)                        in.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (SendInput(1, &in, sizeof(INPUT)) == 0) {
        reportFailureOnce(QString("SendInput refused (error %1)").arg(GetLastError()));
    }
}

void InputInjector::handlePacket(const QByteArray& json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonObject obj = doc.object();
    const auto type = static_cast<InputEventType>(obj.value("type").toInt());
    const double x = obj.value("x").toDouble();
    const double y = obj.value("y").toDouble();
    const auto keyCode = static_cast<uint16_t>(obj.value("keyCode").toInt());
    const double deltaX = obj.value("deltaX").toDouble();
    const double deltaY = obj.value("deltaY").toDouble();
    const auto eventId = static_cast<quint64>(obj.value("eventId").toVariant().toULongLong());

    // Commands are control-plane, not input — handle before the dedupe so a
    // repeated keyframe request is never swallowed.
    if (type == InputEventType::Command) {
        switch (keyCode) {
            case kHeartbeatKeyCode:  emit heartbeatReceived(); break;
            case kIDRRequestKeyCode: emit keyframeRequested(); break;
            case 777:                emit screenInfoReceived(static_cast<int>(deltaX),
                                                             static_cast<int>(deltaY)); break;
            default: break;
        }
        return;
    }

    if (isDuplicate(eventId)) return;

    switch (type) {
        case InputEventType::MouseMove:
            injectMouse(x, y, 0);
            break;
        case InputEventType::LeftMouseDown:
            injectMouse(x, y, MOUSEEVENTF_LEFTDOWN);
            break;
        case InputEventType::LeftMouseUp:
            injectMouse(x, y, MOUSEEVENTF_LEFTUP);
            break;
        case InputEventType::RightMouseDown:
            injectMouse(x, y, MOUSEEVENTF_RIGHTDOWN);
            break;
        case InputEventType::RightMouseUp:
            injectMouse(x, y, MOUSEEVENTF_RIGHTUP);
            break;
        case InputEventType::ScrollWheel:
            injectScroll(x, y, deltaX, deltaY, keyCode);
            break;
        case InputEventType::KeyDown:
            injectKey(keyCode, true);
            break;
        case InputEventType::KeyUp:
            injectKey(keyCode, false);
            break;
        default:
            break;
    }
}
