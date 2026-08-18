#include "HotspotManager.h"

#ifdef _WIN32

// WinRT headers must come before Qt's: <windows.h> arrives with them and
// defines an "interface" macro that Qt headers otherwise trip over.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

#endif // _WIN32

#include <QMetaObject>
#include <QPointer>

#include <stdexcept>
#include <thread>

#ifdef _WIN32
using namespace winrt;
using namespace winrt::Windows::Networking::Connectivity;
using namespace winrt::Windows::Networking::NetworkOperators;

namespace {

QString hstr(const winrt::hstring& s) {
    return QString::fromWCharArray(s.c_str(), static_cast<int>(s.size()));
}

// Build a manager for whatever connection is currently providing internet.
// Throws if there is no such profile or the platform refuses — every caller
// runs this inside a try/catch and reports through Info::error.
//
// std::runtime_error rather than winrt::hresult_error for the missing-profile
// case: the HRESULT constants live in winerror.h, which WIN32_LEAN_AND_MEAN
// keeps out, and inventing an HRESULT to describe "the user has no network"
// buys nothing over a plain message.
NetworkOperatorTetheringManager makeManager() {
    auto profile = NetworkInformation::GetInternetConnectionProfile();
    if (!profile) {
        throw std::runtime_error("No active network connection to share");
    }
    return NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
}

} // namespace
#endif // _WIN32

HotspotManager::HotspotManager(QObject* parent) : QObject(parent) {}
HotspotManager::~HotspotManager() = default;

HotspotManager::Info HotspotManager::query() const {
    Info info;
#ifdef _WIN32
    try {
        auto mgr = makeManager();
        info.supported   = true;
        info.on          = mgr.TetheringOperationalState() == TetheringOperationalState::On;
        info.clientCount = static_cast<int>(mgr.ClientCount());
        info.maxClients  = static_cast<int>(mgr.MaxClientCount());

        auto cfg = mgr.GetCurrentAccessPointConfiguration();
        info.ssid       = hstr(cfg.Ssid());
        info.passphrase = hstr(cfg.Passphrase());
    } catch (const winrt::hresult_error& e) {
        info.supported = false;
        info.error     = hstr(e.message());
    } catch (const std::exception& e) {
        info.supported = false;
        info.error     = QString::fromUtf8(e.what());
    } catch (...) {
        info.supported = false;
        info.error     = QStringLiteral("Unknown error querying Mobile Hotspot");
    }
#else
    info.error = QStringLiteral("Mobile Hotspot is a Windows feature");
#endif
    return info;
}

#ifdef _WIN32
namespace {

// Run one tethering operation on its own thread.
//
// StartTetheringAsync/StopTetheringAsync are awaited with a blocking get(),
// which would deadlock if called on Qt's GUI thread — Qt initialises that one
// as an STA for drag-and-drop, and a blocking wait there stalls the very
// message pump the completion needs. A dedicated thread declaring itself
// multi-threaded avoids the whole problem.
template <typename Op>
void runOffThread(QPointer<HotspotManager> owner, Op op) {
    std::thread([owner, op]() {
        QString error;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto mgr    = makeManager();
            auto result = op(mgr).get();
            if (result.Status() != TetheringOperationStatus::Success) {
                auto extra = hstr(result.AdditionalErrorMessage());
                error = extra.isEmpty()
                    ? QStringLiteral("Mobile Hotspot refused the request (status %1)")
                          .arg(static_cast<int>(result.Status()))
                    : extra;
            }
        } catch (const winrt::hresult_error& e) {
            error = hstr(e.message());
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        } catch (...) {
            error = QStringLiteral("Unknown Mobile Hotspot error");
        }

        // Back to the owner's thread. If it has already gone there is nothing
        // to deliver to — invokeMethod needs a real context object, never null.
        auto* target = owner.data();
        if (!target) return;
        QMetaObject::invokeMethod(
            target,
            [owner, error]() {
                if (!owner) return;
                if (error.isEmpty()) emit owner->stateChanged(owner->query());
                else                 emit owner->failed(error);
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace
#endif // _WIN32

void HotspotManager::start() {
#ifdef _WIN32
    runOffThread(QPointer<HotspotManager>(this),
                 [](NetworkOperatorTetheringManager& m) { return m.StartTetheringAsync(); });
#else
    emit failed(QStringLiteral("Mobile Hotspot is a Windows feature"));
#endif
}

void HotspotManager::stop() {
#ifdef _WIN32
    runOffThread(QPointer<HotspotManager>(this),
                 [](NetworkOperatorTetheringManager& m) { return m.StopTetheringAsync(); });
#else
    emit failed(QStringLiteral("Mobile Hotspot is a Windows feature"));
#endif
}
