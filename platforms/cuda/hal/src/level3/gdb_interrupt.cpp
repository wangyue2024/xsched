#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xsched/cuda/hal/level3/gdb_interrupt.h"
#include "xsched/utils/log.h"
#include "xsched/utils/xassert.h"

using namespace xsched::cuda;

namespace
{

double NowSeconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/// \brief Minimal cuda-gdb MI client.
///
/// cuda-gdb 13.2 has no Python and its MI state tracking is fragile right after
/// attach, so the commands are paced and the result records are matched
/// explicitly instead of relying on the MI state machine.
class MiClient
{
public:
    ~MiClient() { Kill(); }

    bool Spawn()
    {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execlp("cuda-gdb", "cuda-gdb", "--interpreter=mi2", "-q", "-nx", nullptr);
            _exit(127);
        }
        close(in[0]); close(out[1]);
        to_ = in[1]; from_ = out[0];
        return WaitFor("(gdb)", 30.0);
    }

    bool Attach(pid_t pid)
    {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "-target-attach %d\n", (int)pid);
        if (write(to_, cmd, strlen(cmd)) < 0) return false;
        return WaitFor("*stopped", 60.0);   // the attach stop == device suspended
    }

    bool Detach()
    {
        if (!attached_) return true;
        const char *cmd = "-target-detach\n";
        if (write(to_, cmd, strlen(cmd)) < 0) return false;
        // gdb answers ^done on success and ^error "The program is not being run."
        // when the inferior already exited - both mean we are no longer attached.
        double t0 = NowSeconds();
        while (NowSeconds() - t0 < 10.0) {
            size_t nl;
            while ((nl = buf_.find('\n')) != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                if (line.rfind("^done", 0) == 0 || line.rfind("^error", 0) == 0) {
                    attached_ = false;
                    return true;
                }
            }
            if (!Read(50)) break;
        }
        return false;
    }

    void Kill()
    {
        if (to_ >= 0) { const char *c = "-gdb-exit\n"; ssize_t w = write(to_, c, strlen(c)); (void)w; }
        if (pid_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
        if (to_ >= 0) close(to_);
        if (from_ >= 0) close(from_);
        to_ = from_ = -1;
    }

    bool IsAlive() { return pid_ > 0 && kill(pid_, 0) == 0; }
    bool IsAttached() { return attached_; }
    void MarkAttached() { attached_ = true; }

private:
    bool Read(int timeout_ms)
    {
        struct pollfd p;
        p.fd = from_; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        char tmp[4096];
        ssize_t n = read(from_, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf_.append(tmp, (size_t)n);
        return true;
    }

    bool WaitFor(const char *prefix, double timeout_s)
    {
        double t0 = NowSeconds();
        while (NowSeconds() - t0 < timeout_s) {
            size_t nl;
            while ((nl = buf_.find('\n')) != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                if (line.rfind(prefix, 0) == 0) return true;
            }
            if (!Read(50)) continue;
        }
        return false;
    }

    pid_t pid_ = -1;
    int to_ = -1, from_ = -1;
    bool attached_ = false;
    std::string buf_;
};

} // namespace

struct GdbInterruptContext::Impl
{
    pid_t pid = -1;
    std::shared_ptr<MiClient> mi;
};

GdbInterruptContext &GdbInterruptContext::Instance()
{
    static GdbInterruptContext instance;
    return instance;
}

GdbInterruptContext::~GdbInterruptContext() { DetachAll(); }

bool GdbInterruptContext::AllowDebuggerAttach()
{
    return prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) == 0;
}

bool GdbInterruptContext::IsAvailable()
{
    const char *path = getenv("PATH");
    if (path == nullptr) return false;
    std::string paths(path);
    size_t begin = 0;
    while (begin <= paths.size()) {
        size_t end = paths.find(':', begin);
        std::string dir = paths.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        std::string candidate = dir + "/cuda-gdb";
        if (access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

bool GdbInterruptContext::Prepare(pid_t pid)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = agents_.find(pid);
    if (it != agents_.end() && it->second->mi && it->second->mi->IsAlive()) return true;

    auto impl = std::make_shared<Impl>();
    impl->pid = pid;
    impl->mi = std::make_shared<MiClient>();
    if (!impl->mi->Spawn()) {
        XWARN("gdb interrupt: cannot spawn cuda-gdb for pid %d", (int)pid);
        return false;
    }
    XINFO("gdb interrupt: agent prepared for pid %d", (int)pid);
    agents_[pid] = impl;
    return true;
}

bool GdbInterruptContext::Suspend(pid_t pid)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = agents_.find(pid);
    if (it != agents_.end() && it->second->mi && it->second->mi->IsAlive()
        && it->second->mi->IsAttached()) return false;

    // reuse a pre-spawned agent when available (Prepare())
    auto impl = (it != agents_.end()) ? it->second : std::make_shared<Impl>();
    impl->pid = pid;
    if (!impl->mi) impl->mi = std::make_shared<MiClient>();
    if (!impl->mi->IsAlive() && !impl->mi->Spawn()) {
        XWARN("gdb interrupt: cannot spawn cuda-gdb for pid %d", (int)pid);
        return false;
    }
    double t0 = NowSeconds();
    if (!impl->mi->Attach(pid)) {
        XWARN("gdb interrupt: attach to pid %d failed (is PR_SET_PTRACER set?)", (int)pid);
        impl->mi->Kill();
        return false;
    }
    XINFO("gdb interrupt: pid %d suspended in %.3f s", (int)pid, NowSeconds() - t0);
    impl->mi->MarkAttached();
    agents_[pid] = impl;
    return true;
}

bool GdbInterruptContext::Resume(pid_t pid)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = agents_.find(pid);
    if (it == agents_.end()) return false;
    bool ok = it->second->mi->Detach();
    it->second->mi->Kill();
    agents_.erase(it);
    if (ok) {
        XINFO("gdb interrupt: pid %d resumed", (int)pid);
    } else {
        XWARN("gdb interrupt: pid %d detach reported an error", (int)pid);
    }
    return ok;
}

GdbInterruptContext::State GdbInterruptContext::GetState(pid_t pid)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = agents_.find(pid);
    if (it == agents_.end()) return State::kRunning;
    return it->second->mi->IsAlive() ? State::kSuspended : State::kRunning;
}

void GdbInterruptContext::DetachAll()
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto &kv : agents_) {
        if (kv.second->mi) { kv.second->mi->Detach(); kv.second->mi->Kill(); }
    }
    agents_.clear();
}
