#include <tensorpipe/transport/shm/loop.h>

#include <sys/eventfd.h>

#include <tensorpipe/common/defs.h>

namespace tensorpipe {
namespace transport {
namespace shm {

namespace {

// Checks if the specified weak_ptr is uninitialized.
template <typename T>
bool is_uninitialized(const std::weak_ptr<T>& weak) {
  const std::weak_ptr<T> empty{};
  return !weak.owner_before(empty) && !empty.owner_before(weak);
}

} // namespace

FunctionEventHandler::FunctionEventHandler(
    std::shared_ptr<Loop> loop,
    int fd,
    int event,
    TFunction fn)
    : loop_(std::move(loop)), fd_(fd), event_(event), fn_(std::move(fn)) {}

FunctionEventHandler::~FunctionEventHandler() {
  cancel();
}

void FunctionEventHandler::start() {
  loop_->registerDescriptor(fd_, event_, shared_from_this());
}

void FunctionEventHandler::cancel() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!cancelled_) {
    loop_->unregisterDescriptor(fd_);
    cancelled_ = true;
  }
}

void FunctionEventHandler::handleEvents(int events) {
  if (events & event_) {
    fn_(*this);
  }
}

std::shared_ptr<Loop> Loop::create() {
  return std::make_shared<Loop>(ConstructorToken());
}

Loop::Loop(ConstructorToken /* unused */) {
  {
    auto rv = epoll_create(1);
    TP_THROW_SYSTEM_IF(rv == -1, errno);
    epollFd_ = Fd(rv);
  }
  {
    auto rv = eventfd(0, EFD_NONBLOCK);
    TP_THROW_SYSTEM_IF(rv == -1, errno);
    eventFd_ = Fd(rv);
  }

  // Register for readability on eventfd(2).
  // The user data is left empty here because reading from the
  // eventfd is special cased in the loop's body.
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = eventFd_.fd();
  handlers_.resize(eventFd_.fd() + 1);
  auto rv = epoll_ctl(epollFd_.fd(), EPOLL_CTL_ADD, eventFd_.fd(), &ev);
  TP_THROW_SYSTEM_IF(rv == -1, errno);

  // Start epoll(2) thread.
  loop_.reset(new std::thread(&Loop::loop, this));
}

Loop::~Loop() {
  TP_DCHECK(done_);
}

void Loop::registerDescriptor(
    int fd,
    int events,
    std::shared_ptr<EventHandler> h) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;

  {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    if (fd >= handlers_.size()) {
      handlers_.resize(fd + 1);
    }
    if (is_uninitialized(handlers_[fd])) {
      handlerCount_++;
    }
    handlers_[fd] = h;
  }

  auto rv = epoll_ctl(epollFd_.fd(), EPOLL_CTL_ADD, fd, &ev);
  if (rv == -1 && errno == EEXIST) {
    rv = epoll_ctl(epollFd_.fd(), EPOLL_CTL_MOD, fd, &ev);
  }
  TP_THROW_SYSTEM_IF(rv == -1, errno);
}

void Loop::unregisterDescriptor(int fd) {
  auto rv = epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, fd, nullptr);
  TP_THROW_SYSTEM_IF(rv == -1, errno);

  {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    if (!is_uninitialized(handlers_[fd])) {
      handlerCount_--;
    }
    handlers_[fd].reset();
  }
}

std::future<void> Loop::run(TFunction fn) {
  std::unique_lock<std::mutex> lock(m_);

  // Must use a copyable wrapper around std::promise because
  // we use it from a std::function which must be copyable.
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  functions_.push_back(
      [promise{std::move(promise)}, fn{std::move(fn)}]() mutable {
        try {
          fn();
          promise->set_value();
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      });
  wakeup();
  return future;
}

void Loop::wakeup() {
  // Perform a write to eventfd to wake up epoll_wait(2).
  eventFd_.writeOrThrow<uint64_t>(1);
}

void Loop::loop() {
  std::array<struct epoll_event, capacity_> events;
  for (;;) {
    auto nfds = epoll_wait(epollFd_.fd(), events.data(), events.size(), -1);
    if (nfds == -1) {
      if (errno == EINTR) {
        continue;
      }
      TP_THROW_SYSTEM(errno);
    }

    // Process events returned by epoll_wait(2).
    {
      std::unique_lock<std::mutex> lock(handlersMutex_);
      for (auto i = 0; i < nfds; i++) {
        const auto& event = events[i];
        const auto fd = event.data.fd;
        auto h = handlers_[fd].lock();
        if (h) {
          lock.unlock();
          // Trigger callback. Note that the object is kept alive
          // through the shared_ptr that we acquired by locking the
          // weak_ptr in the handlers vector.
          h->handleEvents(events[i].events);
          // Reset the handler shared_ptr before reacquiring the lock.
          // This may trigger destruction of the object.
          h.reset();
          lock.lock();
        }
      }
    }

    // Process deferred functions. Note that we keep continue running
    // until there are no more functions remaining. This is necessary
    // such that we can assert in the block below that if there are no
    // more handlers, we are done.
    {
      std::unique_lock<std::mutex> lock(m_);
      while (!functions_.empty()) {
        decltype(functions_) stackFunctions;
        std::swap(stackFunctions, functions_);
        lock.unlock();

        // Run deferred functions.
        for (auto& fn : stackFunctions) {
          fn();

          // Reset function to destroy resources associated with it
          // before re-acquiring the lock.
          fn = nullptr;
        }

        lock.lock();
      }
    }

    // Return if another thread is waiting in `join` and there is
    // nothing left to be done.
    if (done_ && handlerCount_ == 0) {
      return;
    }
  }
}

void Loop::join() {
  done_ = true;
  wakeup();
  loop_->join();
}

} // namespace shm
} // namespace transport
} // namespace tensorpipe