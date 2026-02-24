#include <vulkan/vulkan.h>
#include <wayland-client.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>
#else
#error "Unsupported Target"
#endif

#include <spdlog/spdlog.h>

#include <map>
#define MAX_EVENTS 10

template<typename T>
using UniqueCPtr = std::unique_ptr<T, void (*)(T *)>;

struct EventCallback
{
    void (*callback)(int, void *);
    void *userdata;
};

class EventLoop
{
    int _epollfd;

    std::map<int, EventCallback> _registered;

public:
    EventLoop() : _registered()
    {
        int fd = epoll_create1(0);
        if (fd == -1) { throw std::system_error(errno, std::system_category()); }

        this->_epollfd = fd;
    }

    template<typename T>
    void insert(int fd, void (*callback)(int, void *), T *userdata)
    {
        epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = fd;

        if (epoll_ctl(_epollfd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            throw std::system_error(errno, std::system_category());
        }

        _registered.insert_or_assign(
          fd,
          EventCallback { .callback = callback, .userdata = static_cast<void *>(userdata) });
    }

    void dispatch_next()
    {
        epoll_event events[MAX_EVENTS];

        int nfds = epoll_wait(_epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) { throw std::system_error(errno, std::system_category()); }

        for (int n = 0; n < nfds; n++)
        {
            struct epoll_event &ev    = events[n];
            EventCallback      &event = _registered.at(ev.data.fd);

            event.callback(ev.data.fd, event.userdata);
        }
    }
};

class WindowState;

class WindowState
{
    std::vector<int>          _required_names;
    UniqueCPtr<wl_compositor> _compositor;

    static constexpr int wl_compositor_version = 4;

public:
    bool active;

    WindowState() : _compositor(nullptr, [](wl_compositor *) { }), active(true) { }

    void Close()
    {
        _compositor = UniqueCPtr<wl_compositor>(nullptr, [](wl_compositor *) { });
        active      = false;
    }

    void RegistryHandleGlobal(
      wl_registry     *registry,
      uint32_t         name,
      std::string_view interface,
      uint32_t         version) noexcept
    {
        spdlog::debug("Global Add -> {}: {} (Name {})", interface, version, name);

        if (interface.compare(wl_compositor_interface.name) == 0)
        {
            _compositor = UniqueCPtr<wl_compositor>(
              static_cast<struct wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, wl_compositor_version)),
              wl_compositor_destroy);
            _required_names.push_back(name);
        }
    }

    void RegistryHandleGlobalRemove(wl_registry *, uint32_t name)
    {
        if (std::binary_search(_required_names.begin(), _required_names.end(), name))
        {
            throw std::runtime_error("Compositor removed required capability");
        }
    }

public:
    static const wl_registry_listener registry_listener;
};

const wl_registry_listener WindowState::registry_listener = {
    .global =
      [](
        void        *userdata,
        wl_registry *registry,
        uint32_t     name,
        const char  *interface,
        uint32_t     version)
    {
        WindowState *state = static_cast<WindowState *>(userdata);
        state->RegistryHandleGlobal(registry, name, std::string_view(interface), version);
    },
    .global_remove =
      [](void *userdata, wl_registry *registry, uint32_t name)
    {
        WindowState *state = static_cast<WindowState *>(userdata);
        state->RegistryHandleGlobalRemove(registry, name);
    },
};

int main()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    // sigaddset(&mask, SIGTERM);
    int r = sigprocmask(SIG_BLOCK, &mask, nullptr);
    if (r == -1) { throw std::system_error(errno, std::system_category()); }

    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sigfd == -1) { throw std::system_error(errno, std::system_category()); }

    spdlog::info("Hello World!");
    spdlog::enable_backtrace(32);

    spdlog::set_level(spdlog::level::debug);

    UniqueCPtr<wl_display> display(wl_display_connect(nullptr), wl_display_disconnect);
    if (display.get() == nullptr)
    {
        spdlog::error("Failed to connecct to Wayland Server");
        return 1;
    }
    spdlog::info("Connection established!");

    int epollfd = epoll_create1(0);
    if (epollfd == -1)
    {
        spdlog::debug(strerror(errno));
        spdlog::error("Failed to create epoll instance");
        return 1;
    }

    WindowState state;

    EventLoop el = EventLoop();
    el.insert(
      sigfd,
      [](int fd, void *userdata)
      {
          WindowState *state = static_cast<WindowState *>(userdata);

          signalfd_siginfo siginfo;
          while (true)
          {
              int res = read(fd, &siginfo, sizeof(signalfd_siginfo));
              if (res == -1)
              {
                  int errnum = errno;
                  if (errnum == EAGAIN) { break; }

                  throw std::system_error(errnum, std::system_category());
              }

              spdlog::info("Exit signal captured!");
              state->Close();
          }
      },
      &state);
    el.insert(
      wl_display_get_fd(display.get()),
      [](int, void *ptr) { wl_display_dispatch(static_cast<wl_display *>(ptr)); },
      display.get());

    UniqueCPtr<wl_registry> registry(
      wl_display_get_registry(display.get()),
      wl_registry_destroy);    // I think this is safe to do?

    wl_registry_add_listener(registry.get(), &WindowState::registry_listener, &state);
    wl_display_roundtrip(display.get());

    while (state.active) { el.dispatch_next(); }

    return 0;
}
