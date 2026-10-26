// l3mi.cpp - a minimal cuda-gdb MI driver: the XSched level-3 interrupt agent.
//
// The RM debugger-session route is closed to us (see FINDINGS_ZH.md v0.3), but
// cuda-gdb's own stack is not: driving it in MI mode gives us
//   -exec-interrupt  -> stop the inferior, which suspends the device mid-kernel
//   -exec-continue   -> resume
// with no cooperation from the target other than prctl(PR_SET_PTRACER, ANY),
// and *no* breakpoint in the target's code.
//
// Verified freeze windows (evidence/live/mi*_watch.log): a 4 s interrupt hold
// produced a 1.88 s observed freeze in a 24 ms-sampled external counter.
//
// Build: ../build.sh l3mi     Run:
//   ./l3mi --pid <target> [--cycles 3] [--hold-ms 1000] [--gap-ms 1000]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <chrono>
#include <time.h>
#include <poll.h>

static double now_s()
{
    // seconds since boot: lets us line the agent log up with shm_watch's clock
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct MiAgent {
    pid_t gdb = -1;
    int   to_gdb = -1, from_gdb = -1;
    std::string buf;

    bool spawn()
    {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) { perror("pipe"); return false; }
        gdb = fork();
        if (gdb < 0) { perror("fork"); return false; }
        if (gdb == 0) {
            dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execlp("cuda-gdb", "cuda-gdb", "--interpreter=mi2", "-q", "-nx", nullptr);
            _exit(127);
        }
        close(in[0]); close(out[1]);
        to_gdb = in[1]; from_gdb = out[0];
        return true;
    }

    // drain gdb's output until a line starting with `prefix` is seen
    bool wait_for(const char *prefix, double timeout_s = 30.0)
    {
        double t0 = now_s();
        char tmp[4096];
        for (;;) {
            size_t nl = buf.find('\n');
            while (nl != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.rfind(prefix, 0) == 0) {
                    printf("[mi] %s\n", line.c_str());
                    return true;
                }
                if (line.rfind("^", 0) == 0)
                    printf("[mi] %s\n", line.c_str());
                nl = buf.find('\n');
            }
            if (now_s() - t0 > timeout_s) {
                printf("[mi] TIMEOUT waiting for '%s'\n", prefix);
                return false;
            }
            struct pollfd p; p.fd = from_gdb; p.events = POLLIN; p.revents = 0;
            if (poll(&p, 1, 50) <= 0) continue;
            ssize_t n = read(from_gdb, tmp, sizeof tmp);
            if (n <= 0) { printf("[mi] gdb closed\n"); return false; }
            buf.append(tmp, (size_t)n);
        }
    }

    // consume whatever is already queued so a stale "*stopped"/"^running" from
    // the previous command cannot satisfy the next wait
    void drain(int quiet_ms = 60)
    {
        char tmp[4096];
        for (;;) {
            struct pollfd p; p.fd = from_gdb; p.events = POLLIN; p.revents = 0;
            if (poll(&p, 1, quiet_ms) <= 0) return;
            ssize_t n = read(from_gdb, tmp, sizeof tmp);
            if (n <= 0) return;
            buf.append(tmp, (size_t)n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) buf.erase(0, nl + 1);
        }
    }

    bool cmd(const char *c, const char *expect)
    {
        std::string line = std::string(c) + "\n";
        if (write(to_gdb, line.data(), line.size()) < 0) return false;
        bool ok = wait_for(expect);
        drain();
        return ok;
    }

    // cuda-gdb's MI state tracking is fragile right after attach; pace the
    // commands and drain in the background instead of relying on result records
    // (the token-prefixed shell driver in run_l3mi_mi.sh proved this works).
    bool attach(pid_t pid) {
        char c[64]; snprintf(c, sizeof c, "-target-attach %d\n", (int)pid);
        if (write(to_gdb, c, strlen(c)) < 0) return false;
        return wait_for("*stopped", 40.0);      // the attach stop = device frozen
    }
    bool interrupt() {
        const char *c = "-exec-interrupt\n";
        if (write(to_gdb, c, strlen(c)) < 0) return false;
        return wait_for("*stopped", 20.0);
    }
    // gdb reports "^error,msg=The program is not being run." when the inferior
    // already exited, which is not a failure for us - treat both as detached.
    bool detach()
    {
        const char *c = "-target-detach\n";
        if (write(to_gdb, c, strlen(c)) < 0) return false;
        double t0 = now_s();
        char tmp[4096];
        while (now_s() - t0 < 5.0) {
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.rfind("^done", 0) == 0 || line.rfind("^error", 0) == 0) {
                    printf("[mi] %s\n", line.c_str());
                    return true;
                }
            }
            struct pollfd p; p.fd = from_gdb; p.events = POLLIN; p.revents = 0;
            if (poll(&p, 1, 50) <= 0) continue;
            ssize_t n = read(from_gdb, tmp, sizeof tmp);
            if (n <= 0) break;
            buf.append(tmp, (size_t)n);
        }
        return false;
    }
    bool reattach(pid_t pid)   { return attach(pid); }
    bool cont() {
        const char *c = "-exec-continue\n";
        if (write(to_gdb, c, strlen(c)) < 0) return false;
        if (!wait_for("^running", 10.0)) return false;
        drain(150);                              // let the *running events settle
        return true;
    }
    void quit() {
        if (to_gdb >= 0) { const char *c = "-gdb-exit\n"; ssize_t w = write(to_gdb, c, strlen(c)); (void)w; }
        if (gdb > 0) { usleep(300000); kill(gdb, SIGTERM); waitpid(gdb, nullptr, 0); gdb = -1; }
    }
};

// ---------------------------------------------------------------- daemon mode
// The control block a scheduler process maps:
//   cmd    : 0 idle, 1 suspend, 2 resume, 3 quit
//   state  : 0 running, 1 suspended   (written by the agent)
//   seq    : incremented by the agent on every state change
struct L3Ctrl {
    volatile unsigned int cmd;
    volatile unsigned int state;
    volatile unsigned int seq;
    volatile int          last_status;
    unsigned int          pad;
};

static int serve(const char *shmname, pid_t pid)
{
    int fd = shm_open(shmname, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, sizeof(L3Ctrl))) { perror("ftruncate"); return 1; }
    L3Ctrl *c = (L3Ctrl *)mmap(nullptr, sizeof(L3Ctrl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c == MAP_FAILED) { perror("mmap"); return 1; }
    memset((void *)c, 0, sizeof *c);
    printf("[l3agentd] serving pid=%d on /dev/shm%s (cmd: 1=suspend 2=resume 3=quit)\n",
           (int)pid, shmname);

    for (;;) {
        unsigned cmd = c->cmd;
        if (cmd == 0) { usleep(500); continue; }
        if (cmd == 3) { printf("[l3agentd] quit\n"); break; }
        c->cmd = 0;
        if (cmd == 1 && c->state == 0) {
            MiAgent mi;
            double t0 = now_s();
            if (!mi.spawn()) { c->last_status = -1; c->seq++; continue; }
            mi.wait_for("(gdb)");
            bool ok = mi.attach(pid);
            double dt = now_s() - t0;
            c->last_status = ok ? 0 : -2;
            if (ok) { c->state = 1; c->seq++; }
            printf("[l3agentd] SUSPEND  ok=%d  %.3f s\n", (int)ok, dt);
            // the gdb process must stay alive while suspended
            if (ok) {
                while (c->cmd != 2 && c->cmd != 3) usleep(500);
                double t1 = now_s();
                mi.detach();
                printf("[l3agentd] RESUME   %.3f s\n", now_s() - t1);
                mi.quit();
                c->state = 0; c->seq++;
                c->cmd = (c->cmd == 3) ? 3 : 0;
                if (c->cmd == 3) break;
            } else {
                mi.quit();
            }
        } else if (cmd == 2 && c->state == 1) {
            c->state = 0; c->seq++;      // handled inside the suspend branch
        }
    }
    munmap(c, sizeof(L3Ctrl));
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    pid_t pid = -1;
    int cycles = 3, hold_ms = 1000, gap_ms = 1000;
    const char *serve_shm = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return std::string(argv[++i]); };
        if (a == "--pid") pid = atoi(next().c_str());
        else if (a == "--cycles") cycles = atoi(next().c_str());
        else if (a == "--hold-ms") hold_ms = atoi(next().c_str());
        else if (a == "--gap-ms") gap_ms = atoi(next().c_str());
        else if (a == "--serve") serve_shm = next().c_str();
    }
    if (pid <= 0) { fprintf(stderr, "usage: l3mi --pid N [--serve SHM | --cycles C --hold-ms MS]\n"); return 2; }
    if (serve_shm) return serve(serve_shm, pid);

    // One attach = one freeze; cuda-gdb does not re-suspend the device on a
    // second -target-attach within the same session, so each intervention gets
    // its own agent process.
    for (int i = 1; i <= cycles; i++) {
        usleep((useconds_t)gap_ms * 1000);
        MiAgent mi;
        if (!mi.spawn()) return 1;
        mi.wait_for("(gdb)");
        printf("[l3mi t=%.4f] cycle %d: attach requested\n", now_s(), i);
        double a = now_s();
        if (!mi.attach(pid)) { fprintf(stderr, "[l3mi] cycle %d attach failed\n", i); mi.quit(); return 1; }
        printf("[l3mi t=%.4f] cycle %d: FREEZE (attach %.3f s) — holding %d ms\n", now_s(), i, now_s() - a, hold_ms);
        usleep((useconds_t)hold_ms * 1000);
        a = now_s();
        mi.detach();
        printf("[l3mi t=%.4f] cycle %d: RESUME (detach %.3f s)\n", now_s(), i, now_s() - a);
        mi.quit();
    }
    printf("[l3mi] done\n");
    return 0;
}
