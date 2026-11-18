#include "net/platform.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace {
bool g_wsa_ready = false;
bool g_timer_raised = false;
}
#endif

namespace mcast::net {

bool InitSockets(std::string& err) {
#ifdef _WIN32
    if (g_wsa_ready) return true;
    WSADATA wsa{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        err = "WSAStartup failed: " + SocketErrorText(rc);
        return false;
    }
    g_wsa_ready = true;
    // Windows' default timer granularity is ~15.6 ms; the send pacer needs
    // millisecond sleeps to hold a stable bitrate.
    timeBeginPeriod(1);
    g_timer_raised = true;
    return true;
#else
    (void)err;
    return true;
#endif
}

void ShutdownSockets() {
#ifdef _WIN32
    if (g_timer_raised) {
        timeEndPeriod(1);
        g_timer_raised = false;
    }
    if (g_wsa_ready) {
        WSACleanup();
        g_wsa_ready = false;
    }
#endif
}

void PreciseSleep(int64_t microseconds) {
    if (microseconds <= 0) return;
#ifdef _WIN32
    struct TimerHolder {
        HANDLE handle = nullptr;
        TimerHolder() {
            handle = CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
        }
        ~TimerHolder() {
            if (handle) CloseHandle(handle);
        }
    };
    thread_local TimerHolder timer;
    if (timer.handle) {
        LARGE_INTEGER due{};
        due.QuadPart = -microseconds * 10;  // 100 ns units, relative
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.handle, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

std::string SocketErrorText(int code) {
#ifdef _WIN32
    char* buffer = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPSTR>(&buffer), 0,
        nullptr);
    std::string text;
    if (len > 0 && buffer) {
        text.assign(buffer, len);
        LocalFree(buffer);
    } else {
        text = "socket error " + std::to_string(code);
    }
#else
    std::string text = std::strerror(code);
#endif
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

}  // namespace mcast::net
