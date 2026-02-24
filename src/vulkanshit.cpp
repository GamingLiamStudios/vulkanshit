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
#include <exception>
#define MAX_EVENTS 10

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
        struct epoll_event ev;
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
        struct epoll_event events[MAX_EVENTS];

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

    std::unique_ptr<struct wl_display, void (*)(struct wl_display *)> display(
      wl_display_connect(nullptr),
      wl_display_disconnect);
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

    EventLoop el = EventLoop();
    el.insert(
      sigfd,
      [](int fd, void *)
      {
          struct signalfd_siginfo siginfo;
          while (true)
          {
              int res = read(fd, &siginfo, sizeof(struct signalfd_siginfo));
              if (res == -1)
              {
                  int errnum = errno;
                  if (errnum == EAGAIN) { break; }

                  throw std::system_error(errnum, std::system_category());
              }

              spdlog::error("Signal captured!");

              // Close safely! ;)
              exit(EXIT_FAILURE);
          }
      },
      (void *) nullptr);
    el.insert(
      wl_display_get_fd(display.get()),
      [](int, void *ptr) { wl_display_dispatch((struct wl_display *) ptr); },
      display.get());

    while (true) { el.dispatch_next(); }

    return 0;
}
