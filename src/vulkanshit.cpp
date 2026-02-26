#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#ifdef __linux__
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#else
#error "Unsupported Target"
#endif

#include <spdlog/spdlog.h>

#include <map>
#define MAX_EVENTS 10

template<typename T>
using UniqueCPtr = std::unique_ptr<T, void (*)(T *)>;

template<typename T>
UniqueCPtr<T> DefaultCPtr() noexcept
{
    return std::unique_ptr<T, void (*)(T *)>(nullptr, [](T *) { });
}

class EventLoop;

struct EventCallback
{
    void (*callback)(EventLoop *, int, void *);
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
    void insert(int fd, int events, void (*callback)(EventLoop *, int, void *), T *userdata)
    {
        epoll_event ev;
        ev.events  = events;
        ev.data.fd = fd;

        if (epoll_ctl(_epollfd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            throw std::system_error(errno, std::system_category());
        }

        _registered.insert_or_assign(
          fd,
          EventCallback { .callback = callback, .userdata = static_cast<void *>(userdata) });
    }

    void remove(int fd)
    {
        this->_registered.erase(fd);
        if (epoll_ctl(_epollfd, EPOLL_CTL_DEL, fd, nullptr) == -1)
        {
            throw std::system_error(errno, std::system_category());
        }
    }

    void modify(int fd, int new_events)
    {
        epoll_event ev;
        ev.events  = new_events;
        ev.data.fd = fd;

        if (epoll_ctl(_epollfd, EPOLL_CTL_MOD, fd, &ev) == -1)
        {
            throw std::system_error(errno, std::system_category());
        }
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

            if (ev.events & EPOLLERR)
            {
                // FIXME: Handle errors
            }

            event.callback(this, ev.data.fd, event.userdata);
        }
    }
};

class WaylandError : public std::exception
{
public:
    enum ErrorKind
    {
        MISSING_REQUIRED_FEATURES,
        DISPATCH_FAILED,
    } kind;

    int errsv;

    const char *what() const noexcept override
    {
        switch (this->kind)
        {
        case MISSING_REQUIRED_FEATURES: return "Compositor is missing required features";
        case DISPATCH_FAILED:
            return "wl_display_dispatch_pending failed";    // TODO: Include strerror(errsv)
        }
    }

    static const WaylandError MissingRequiredFeatures;

    WaylandError(ErrorKind kind, int errsv) noexcept : kind(kind), errsv(errsv) { }

private:
    WaylandError(ErrorKind kind) noexcept : kind(kind), errsv(0) { }
};

const WaylandError WaylandError::MissingRequiredFeatures(MISSING_REQUIRED_FEATURES);

class XDGWindowHandle
{
public:
    static constexpr int buffer_count = 2;

private:
    UniqueCPtr<wl_surface> _surface;
    UniqueCPtr<wl_buffer>  _buffers[buffer_count];
    uint8_t                _released_buffers;

    UniqueCPtr<xdg_surface> _xdg_surface;

    void        *_data;
    std::int32_t _data_len;
    int          _active_buffer;

    const std::int32_t _width, _height;

    std::int32_t  _preferred_scale;
    std::uint32_t _preferred_transform;
    // std::set<wl_output> entered;

public:
    /// Safe as long as pointer doesn't move (static position on stack or Heap-Allocated)
    XDGWindowHandle(
      UniqueCPtr<wl_surface>   surface,
      UniqueCPtr<wl_shm_pool> &shm_pool,    // Dropping the shm_pool doesn't dealloc the memory;
                                            // only dropping the buffers will.
      UniqueCPtr<xdg_surface> xdg_surface,
      void                   *data,
      std::int32_t            data_len,
      std::int32_t            width,
      std::int32_t            height)
        : _surface(std::move(surface)),
          _buffers { DefaultCPtr<wl_buffer>(), DefaultCPtr<wl_buffer>() },
          _released_buffers((1 << buffer_count) - 1), _xdg_surface(std::move(xdg_surface)),
          _data(data), _data_len(data_len), _active_buffer(0), _width(width), _height(height)
    {
        wl_surface_add_listener(surface.get(), &surface_listener, this);

        // Assuming WL_SHM_FORMAT_XRGB8888
        const int32_t stride = width * 4;
        for (std::uintptr_t i = 0; i < buffer_count; i++)
        {
            int32_t               offset = stride * height * i;
            UniqueCPtr<wl_buffer> buffer(
              wl_shm_pool_create_buffer(
                shm_pool.get(),
                offset,
                width,
                height,
                stride,
                WL_SHM_FORMAT_XRGB8888),
              wl_buffer_destroy);

            // TODO: Move buffers into seperate subclass
            wl_buffer_add_listener(buffer.get(), &buffer_listener, this);
            wl_buffer_set_user_data(buffer.get(), reinterpret_cast<void *>(i));

            _buffers[i] = std::move(buffer);
        }
    }
    ~XDGWindowHandle() { munmap(_data, _data_len); }

    int           ActiveID() { return this->_active_buffer; }
    std::uint8_t *ActiveBuffer()
    {
        if ((this->_released_buffers & 1 << this->_active_buffer) == 0)
        {
            spdlog::warn(
              "Compositor hasn't released buffer before it was reused. Maybe increase buffer "
              "count.");
            return nullptr;
        }

        const std::int32_t stride = this->_width * 4;
        std::int32_t       offset = stride * this->_height * this->_active_buffer;
        return static_cast<std::uint8_t *>(this->_data) + offset;
    }

    void SwapBuffers()
    {
        UniqueCPtr<wl_buffer> &active_buffer = this->_buffers[this->_active_buffer];
        wl_surface_attach(this->_surface.get(), active_buffer.get(), 0, 0);
        wl_surface_damage(this->_surface.get(), 0, 0, this->_width, this->_height);
        wl_surface_commit(this->_surface.get());

        this->_released_buffers &= ~(1 << this->_active_buffer);

        this->_active_buffer++;
        if (this->_active_buffer >= buffer_count) { this->_active_buffer = 0; }
    }

private:
    static const wl_buffer_listener  buffer_listener;
    static const wl_surface_listener surface_listener;
};

const wl_buffer_listener XDGWindowHandle::buffer_listener {
    .release =
      [](void *userdata, struct wl_buffer *buf)
    {
        XDGWindowHandle *handle  = static_cast<XDGWindowHandle *>(userdata);
        void            *bufdata = wl_buffer_get_user_data(buf);
        handle->_released_buffers |= 1 << reinterpret_cast<std::uintptr_t>(bufdata);
    }
};

const wl_surface_listener XDGWindowHandle::surface_listener = {
    .enter = [](void *, wl_surface *, wl_output *) {},
    .leave = [](void *, wl_surface *, wl_output *) {},
    .preferred_buffer_scale =
      [](void *userdata, wl_surface *, std::int32_t scale)
    {
        XDGWindowHandle *handle  = static_cast<XDGWindowHandle *>(userdata);
        handle->_preferred_scale = scale;
    },
    .preferred_buffer_transform =
      [](void *userdata, wl_surface *, std::uint32_t transform)
    {
        XDGWindowHandle *handle      = static_cast<XDGWindowHandle *>(userdata);
        handle->_preferred_transform = transform;
    }
};

class XDGTopLevelHandle : public XDGWindowHandle
{
    UniqueCPtr<xdg_toplevel> _xdg_toplevel;

public:
    XDGTopLevelHandle(
      UniqueCPtr<wl_surface>   surface,
      UniqueCPtr<wl_shm_pool> &shm_pool,    // Dropping the shm_pool doesn't dealloc the memory;
                                            // only dropping the buffers will.
      UniqueCPtr<xdg_surface> xdg_surface,
      void                   *data,
      std::int32_t            data_len,
      std::int32_t            width,
      std::int32_t            height)
        : XDGWindowHandle(
            std::move(surface),
            shm_pool,
            std::move(xdg_surface),
            data,
            data_len,
            width,
            height),
          _xdg_toplevel(DefaultCPtr<xdg_toplevel>())
    {
        UniqueCPtr<xdg_toplevel> toplevel(
          xdg_surface_get_toplevel(xdg_surface.get()),
          xdg_toplevel_destroy);
        xdg_toplevel_add_listener(toplevel.get(), &toplevel_listener, this);
        _xdg_toplevel = std::move(toplevel);
    }

private:
    static const xdg_toplevel_listener toplevel_listener;
};

const xdg_toplevel_listener XDGTopLevelHandle::toplevel_listener {
    .configure        = [](void *, xdg_toplevel *, std::int32_t, std::int32_t, wl_array *) { },
    .close            = [](void *, xdg_toplevel *) { },
    .configure_bounds = [](void *, xdg_toplevel *, std::int32_t, std::int32_t) { },
    .wm_capabilities  = [](void *, xdg_toplevel *, wl_array *) { },
};

class WindowState
{
    std::vector<int>          _required_names;
    UniqueCPtr<wl_compositor> _compositor;
    UniqueCPtr<wl_shm>        _shm;
    UniqueCPtr<xdg_wm_base>   _xdg_base;

    static constexpr int wl_compositor_version = 4;
    static constexpr int wl_shm_version        = 1;

    // Most of this is from the wayland-book, but modified to my needs
    int AllocateShm(size_t size)
    {
        auto randname = [](char *buf)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long r = ts.tv_nsec;
            for (int i = 0; i < 6; ++i)
            {
                buf[i] = 'A' + (r & 15) + (r & 16) * 2;
                r >>= 5;
            }
        };

        int fd;
        int retries = 100;
        do
        {
            char name[] = "/wl_shm-XXXXXX";
            randname(name + sizeof(name) - 7);
            --retries;

            fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd == -1) { continue; }

            shm_unlink(name);
            break;
        } while (retries > 0 && errno == EEXIST);

        if (fd == -1 && errno != EEXIST) { throw std::system_error(errno, std::system_category()); }
        if (fd == -1) { throw std::runtime_error("Failed to create SHM"); }

        int ret;
        do { ret = ftruncate(fd, size); } while (ret < 0 && errno == EINTR);
        if (ret < 0)
        {
            close(fd);
            throw std::system_error(errno, std::system_category());
        }

        return fd;
    }

public:
    bool active;

    WindowState()
        : _compositor(DefaultCPtr<wl_compositor>()), _shm(DefaultCPtr<wl_shm>()),
          _xdg_base(DefaultCPtr<xdg_wm_base>()), active(true)
    {
    }

    void Close()
    {
        _xdg_base   = DefaultCPtr<xdg_wm_base>();
        _shm        = DefaultCPtr<wl_shm>();
        _compositor = DefaultCPtr<wl_compositor>();

        active = false;
    }

    std::unique_ptr<XDGWindowHandle> CreateWindow(uint32_t width, uint32_t height)
    {
        if (this->_compositor.get() == nullptr) { throw WaylandError::MissingRequiredFeatures; }
        if (this->_shm.get() == nullptr) { throw WaylandError::MissingRequiredFeatures; }
        if (this->_xdg_base.get() == nullptr) { throw WaylandError::MissingRequiredFeatures; }

        UniqueCPtr<wl_surface> surface(
          wl_compositor_create_surface(this->_compositor.get()),
          wl_surface_destroy);

        UniqueCPtr<xdg_surface> xdg_surface(
          xdg_wm_base_get_xdg_surface(this->_xdg_base.get(), surface.get()),
          xdg_surface_destroy);

        // Assuming WL_SHM_FORMAT_XRGB8888
        int32_t stride        = width * 4;
        int32_t shm_pool_size = height * stride * 2;    // double-buffered

        int   shm_fd    = AllocateShm(shm_pool_size);
        void *pool_data = mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

        UniqueCPtr<wl_shm_pool> shm_pool(
          wl_shm_create_pool(this->_shm.get(), shm_fd, shm_pool_size),
          wl_shm_pool_destroy);

        return std::make_unique<XDGWindowHandle>(
          std::move(surface),
          shm_pool,
          std::move(xdg_surface),
          pool_data,
          shm_pool_size,
          width,
          height);
    }

    static const wl_registry_listener registry_listener;

private:
    static const wl_shm_listener      shm_listener;
    static const xdg_wm_base_listener wm_base_listener;

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
              static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, wl_compositor_version)),
              wl_compositor_destroy);
            _required_names.push_back(name);
        }

        if (interface.compare(wl_shm_interface.name) == 0)
        {
            _shm = UniqueCPtr<wl_shm>(
              static_cast<wl_shm *>(
                wl_registry_bind(registry, name, &wl_shm_interface, wl_shm_version)),
              wl_shm_destroy);
            _required_names.push_back(name);

            // Attach listener
            wl_shm_add_listener(_shm.get(), &shm_listener, this);
        }

        if (interface.compare(xdg_wm_base_interface.name) == 0)
        {
            _xdg_base = UniqueCPtr<xdg_wm_base>(
              static_cast<xdg_wm_base *>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 5)),
              xdg_wm_base_destroy);
            _required_names.push_back(name);

            // Attach listener
            xdg_wm_base_add_listener(_xdg_base.get(), &wm_base_listener, this);
        }
    }
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
      [](void *userdata, wl_registry *, uint32_t name)
    {
        WindowState *state = static_cast<WindowState *>(userdata);

        if (std::binary_search(state->_required_names.begin(), state->_required_names.end(), name))
        {
            throw std::runtime_error("Compositor removed required capability");
        }
    },
};

const wl_shm_listener WindowState::shm_listener = {
    .format =
      [](void *userdata, wl_shm *shm, uint32_t format)
    {
        WindowState *state = static_cast<WindowState *>(userdata);
        // TODO: Cache these somewhere
    },
};

const xdg_wm_base_listener WindowState::wm_base_listener = {
    .ping =
      [](void *, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
    {
        spdlog::debug("XdgWmBase Ping ({})", serial);
        xdg_wm_base_pong(xdg_wm_base, serial);
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
      EPOLLIN | EPOLLET,
      [](EventLoop *, int fd, void *userdata)
      {
          WindowState *state = static_cast<WindowState *>(userdata);

          signalfd_siginfo siginfo;
          while (true)
          {
              int res = read(fd, &siginfo, sizeof(signalfd_siginfo));
              if (res == -1)
              {
                  if (errno == EAGAIN) { break; }
                  throw std::system_error(errno, std::system_category());
              }

              spdlog::info("Exit signal captured!");
              state->Close();
          }
      },
      &state);
    el.insert(
      dup(wl_display_get_fd(display.get())),    // Unlikely to fail
      EPOLLIN | EPOLLET,
      [](EventLoop *self, int, void *ptr)
      {
          wl_display *display = static_cast<wl_display *>(ptr);

          // Read events
          if (wl_display_read_events(display) == -1)
          {
              throw std::system_error(errno, std::system_category());
          }
          if (wl_display_dispatch_pending(display) == -1)
          {
              throw WaylandError(WaylandError::DISPATCH_FAILED, errno);
          }

          // Drain unhandled dispatches
          while (wl_display_prepare_read(display) != 0)
          {
              if (errno != EAGAIN) { throw std::system_error(errno, std::system_category()); }

              if (wl_display_dispatch_pending(display) == -1)
              {
                  throw WaylandError(WaylandError::DISPATCH_FAILED, errno);
              }
          }

          // Flush outgoing data
          while (true)
          {
              int res = wl_display_flush(display);
              if (res == 0) break;
              if (res == -1)
              {
                  if (errno == EAGAIN)
                  {
                      self->modify(wl_display_get_fd(display), EPOLLOUT | EPOLLONESHOT);
                      break;
                  }
                  throw std::system_error(errno, std::system_category());
              }
          }
      },
      display.get());
    el.insert(
      wl_display_get_fd(display.get()),
      0,
      [](EventLoop *self, int fd, void *ptr)
      {
          wl_display *display = static_cast<wl_display *>(ptr);
          while (true)
          {
              int res = wl_display_flush(display);
              if (res == 0) break;
              if (res == -1)
              {
                  if (errno == EAGAIN)
                  {
                      self->modify(fd, EPOLLOUT | EPOLLONESHOT);
                      break;
                  }
                  throw std::system_error(errno, std::system_category());
              }
          }
      },
      display.get());

    UniqueCPtr<wl_registry> registry(
      wl_display_get_registry(display.get()),
      wl_registry_destroy);    // I think this is safe to do?

    wl_registry_add_listener(registry.get(), &WindowState::registry_listener, &state);

    int dispatched;
    while ((dispatched = wl_display_roundtrip(display.get())) > 2)
        spdlog::debug("Dispatched {} events", dispatched);

    spdlog::debug("Initalized wayland!");

    // Drain unhandled dispatches (not that there should be any after roundtrip)
    while (wl_display_prepare_read(display.get()) != 0)
    {
        if (errno != EAGAIN) { throw std::system_error(errno, std::system_category()); }

        if (wl_display_dispatch_pending(display.get()) == -1)
        {
            throw WaylandError(WaylandError::DISPATCH_FAILED, errno);
        }
    }

    while (state.active)
    {
        el.dispatch_next();

        // Flush outgoing data (unless we hit a blocking send)
        while (true)
        {
            int res = wl_display_flush(display.get());
            if (res == 0) break;
            if (res == -1)
            {
                if (errno == EAGAIN)
                {
                    el.modify(wl_display_get_fd(display.get()), EPOLLOUT | EPOLLONESHOT);
                    break;
                }
                throw std::system_error(errno, std::system_category());
            }
        }
    }

    return 0;
}
