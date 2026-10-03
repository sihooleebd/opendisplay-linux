#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/encoder.hpp"
#include "opendisplay/pipewire_capture.hpp"
#include "opendisplay/socket.hpp"
#include "opendisplay/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace od {

class Session {
public:
    Session(Options options, std::unique_ptr<DesktopBackend> desktop);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(const Endpoint& endpoint);
    /// Called by the Qt main loop. Returns false after disconnection.
    bool tick();
    void stop();
    /// Abandons a pending portal authorization so a start() still on the stack
    /// unwinds on its own. Must be called from the thread that called start().
    void cancel();

private:
    enum class EventKind { Hello, Touch, Scroll };
    struct Event {
        EventKind kind = EventKind::Touch;
        PhoneInfo phone;
        std::string phase;
        double x = 0;
        double y = 0;
    };

    void receiveLoop();
    void startPipeline(const PhoneInfo& phone);
    void stopPipeline();
    bool send(std::string_view payload);
    /// Writes an already length-prefixed message, so the video path does not
    /// rebuild and recopy each access unit just to add four bytes.
    bool sendFramed(std::string_view framed);
    void queue(Event event);

    Options options_;
    Socket socket_;
    std::unique_ptr<DesktopBackend> desktop_;
    PipeWireCapture capture_;
    std::unique_ptr<Encoder> encoder_;
    std::thread receiver_;
    std::atomic_bool connected_ = false;
    std::mutex sendMutex_;
    std::mutex stateMutex_;
    std::condition_variable helloCondition_;
    std::optional<PhoneInfo> phone_;
    std::optional<PhoneInfo> activePhone_;
    std::deque<Event> events_;
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point reconfigureAfter_{};
    bool pipelineRunning_ = false;
    bool reconfigurePending_ = false;
};

}  // namespace od
