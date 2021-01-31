#include "server.hpp"

#include "globals.hpp"
#include "quality.hpp"
#include "xwindow.hpp"

namespace browservice {

Server::Server(CKey,
    weak_ptr<ServerEventHandler> eventHandler,
    shared_ptr<ViceContext> viceCtx
) {
    REQUIRE_UI_THREAD();
    eventHandler_ = eventHandler;
    state_ = Running;
    nextWindowHandle_ = 1;
    viceCtx_ = viceCtx;
    // Setup is finished in afterConstruct_
}

void Server::shutdown() {
    REQUIRE_UI_THREAD();

    if(state_ == Running) {
        state_ = WaitWindows;
        INFO_LOG("Shutting down server");

        map<uint64_t, shared_ptr<Window>> windows;
        swap(windows, openWindows_);
        for(pair<uint64_t, shared_ptr<Window>> p : windows) {
            p.second->close();
            viceCtx_->closeWindow(p.first);
            REQUIRE(cleanupWindows_.insert(p).second);
        }

        checkCleanupComplete_();
    }
}

uint64_t Server::onViceContextCreateWindowRequest(string& reason) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    if(state_ != Running) {
        reason = "Server is shutting down";
        return 0;
    }

    INFO_LOG("Got request for new window from vice plugin");

    if(
        (int)openWindows_.size() + (int)cleanupWindows_.size()
        >= globals->config->windowLimit
    ) {
        INFO_LOG("Denying window creation due to window limit");
        reason = "Maximum number of concurrent windows exceeded";
        return 0;
    }

    uint64_t handle = nextWindowHandle_++;
    REQUIRE(handle);

    shared_ptr<Window> window = Window::tryCreate(shared_from_this(), handle);
    if(window) {
        REQUIRE(openWindows_.emplace(handle, window).second);
        return handle;
    } else {
        reason = "Creating CEF browser for window failed";
        return 0;
    }
}

void Server::onViceContextCloseWindow(uint64_t window) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    auto it = openWindows_.find(window);
    REQUIRE(it != openWindows_.end());

    uint64_t handle = it->first;
    shared_ptr<Window> windowPtr = it->second;
    openWindows_.erase(it);

    windowPtr->close();
    REQUIRE(cleanupWindows_.emplace(handle, windowPtr).second);
}

void Server::onViceContextResizeWindow(
    uint64_t window, int width, int height
) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    auto it = openWindows_.find(window);
    REQUIRE(it != openWindows_.end());

    it->second->resize(width, height);
}

void Server::onViceContextFetchWindowImage(
    uint64_t window,
    function<void(const uint8_t*, size_t, size_t, size_t)> putImage
) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    auto it = openWindows_.find(window);
    REQUIRE(it != openWindows_.end());

    ImageSlice image = it->second->fetchViewImage();
    if(image.width() < 1 || image.height() < 1) {
        image = ImageSlice::createImage(1, 1);
    }
    putImage(image.buf(), image.width(), image.height(), image.pitch());
}

#define FORWARD_INPUT_EVENT(Name, args, call) \
    void Server::onViceContext ## Name args { \
        REQUIRE_UI_THREAD(); \
        REQUIRE(state_ != ShutdownComplete); \
        \
        auto it = openWindows_.find(window); \
        REQUIRE(it != openWindows_.end()); \
        \
        it->second->send ## Name ## Event call; \
    }

FORWARD_INPUT_EVENT(
    MouseDown,
    (uint64_t window, int x, int y, int button),
    (x, y, button)
)
FORWARD_INPUT_EVENT(
    MouseUp,
    (uint64_t window, int x, int y, int button),
    (x, y, button)
)
FORWARD_INPUT_EVENT(
    MouseMove,
    (uint64_t window, int x, int y),
    (x, y)
)
FORWARD_INPUT_EVENT(
    MouseDoubleClick,
    (uint64_t window, int x, int y, int button),
    (x, y, button)
)
FORWARD_INPUT_EVENT(
    MouseWheel,
    (uint64_t window, int x, int y, int dx, int dy),
    (x, y, dx, dy)
)
FORWARD_INPUT_EVENT(
    MouseLeave,
    (uint64_t window, int x, int y),
    (x, y)
)
FORWARD_INPUT_EVENT(
    KeyDown,
    (uint64_t window, int key),
    (key)
)
FORWARD_INPUT_EVENT(
    KeyUp,
    (uint64_t window, int key),
    (key)
)
FORWARD_INPUT_EVENT(
    LoseFocus,
    (uint64_t window),
    ()
)

void Server::onViceContextNavigate(uint64_t window, int direction) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    auto it = openWindows_.find(window);
    REQUIRE(it != openWindows_.end());

    it->second->navigate(direction);
}

void Server::onViceContextShutdownComplete() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == WaitViceContext);

    state_ = ShutdownComplete;
    INFO_LOG("Server shutdown complete");
    postTask(eventHandler_, &ServerEventHandler::onServerShutdownComplete);
}

void Server::onWindowClose(uint64_t handle) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    auto it = openWindows_.find(handle);
    REQUIRE(it != openWindows_.end());

    shared_ptr<Window> window = it->second;
    openWindows_.erase(it);

    REQUIRE(cleanupWindows_.emplace(handle, window).second);

    viceCtx_->closeWindow(handle);
}

void Server::onWindowCleanupComplete(uint64_t handle) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);

    REQUIRE(cleanupWindows_.erase(handle));
    checkCleanupComplete_();
}

void Server::onWindowViewImageChanged(uint64_t handle) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);
    REQUIRE(openWindows_.count(handle));

    viceCtx_->notifyWindowViewChanged(handle);
}

void Server::onWindowCursorChanged(uint64_t handle, int cursor) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);
    REQUIRE(openWindows_.count(handle));

    viceCtx_->setWindowCursor(handle, cursor);
}

bool Server::onWindowNeedsClipboardButtonQuery(uint64_t handle) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);
    REQUIRE(openWindows_.count(handle));

    return viceCtx_->windowNeedsClipboardButtonQuery(handle);
}

void Server::onWindowCreatePopupRequest(
    uint64_t handle,
    function<shared_ptr<Window>(uint64_t)> accept
) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ != ShutdownComplete);
    REQUIRE(openWindows_.count(handle));

    if(state_ != Running) {
        INFO_LOG(
            "Denying popup window request because the server is shutting down"
        );
        return;
    }

    if(
        (int)openWindows_.size() + (int)cleanupWindows_.size()
        >= globals->config->windowLimit
    ) {
        INFO_LOG("Denying popup window request due to window limit");
        return;
    }

    uint64_t newHandle = nextWindowHandle_++;
    REQUIRE(newHandle);

    INFO_LOG(
        "Sending request for the creation of popup window ", newHandle,
        " (opened by existing window ", handle, ") to the vice plugin"
    );

    string msg;
    if(viceCtx_->requestCreatePopup(handle, newHandle, msg)) {
        INFO_LOG(
            "Popup window creation ", newHandle, " accepted by the vice plugin"
        );

        shared_ptr<Window> newWindow = accept(newHandle);
        REQUIRE(newWindow);
        REQUIRE(openWindows_.emplace(newHandle, newWindow).second);
    } else {
        INFO_LOG(
            "Popup window ", newHandle,
            " creation denied by the vice plugin for reason: ", msg
        );
    }
}

void Server::afterConstruct_(shared_ptr<Server> self) {
    viceCtx_->start(self);
}

void Server::checkCleanupComplete_() {
    if(state_ == WaitWindows && cleanupWindows_.empty()) {
        REQUIRE(openWindows_.empty());
        state_ = WaitViceContext;
        viceCtx_->shutdown();
    }
}

}
