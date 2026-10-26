// l3ctl - command line control for the XSched level-3 debugger backend.
//
//   l3ctl check              # is the backend usable on this machine?
//   l3ctl state <pid>        # running / suspended
//   l3ctl suspend <pid>      # freeze the target; stays in the foreground until
//                            # Ctrl-C (or until `l3ctl resume <pid>`)
//   l3ctl resume <pid>       # ask the agent holding <pid> to detach
//
// `suspend` must keep a process alive because the debugger attachment lives in
// it; a one-shot command would drop the attach as soon as it exits.  The agent
// pid is recorded in /tmp/xsched-l3-<pid>.agent so the other verbs can find it.
//
// The target must have been started with XSCHED_SM120_L3_BACKEND=GDB (which is
// what makes it accept the attach) - see
// platforms/cuda/hal/src/arch/arch.cpp: MaybeAllowGdbBackend().
//
// Measured on RTX 5060 / 595.99.02: suspend 116 ms, resume ~115 ms, and a
// frozen GPU-hogging kernel hands the device to other work (66x in
// test/sm120_l3_trap_v2/run_l3hal.sh).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <string>

#include "xsched/cuda/hal/level3/gdb_interrupt.h"

using namespace xsched::cuda;

static std::string AgentFile(pid_t pid)
{
    char buf[64];
    snprintf(buf, sizeof buf, "/tmp/xsched-l3-%d.agent", (int)pid);
    return std::string(buf);
}

static pid_t ReadAgent(pid_t pid)
{
    FILE *f = fopen(AgentFile(pid).c_str(), "r");
    if (f == nullptr) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v > 0 && kill((pid_t)v, 0) != 0) v = -1;   // stale
    return (pid_t)v;
}

static volatile sig_atomic_t g_stop = 0;
static void OnSignal(int) { g_stop = 1; }

static void Usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s check\n"
            "       %s state   <pid>\n"
            "       %s suspend <pid>\n"
            "       %s resume  <pid>\n", prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { Usage(argv[0]); return 2; }
    const char *cmd = argv[1];

    if (strcmp(cmd, "check") == 0) {
        bool ok = GdbInterruptContext::IsAvailable();
        printf("level-3 debugger backend: %s\n", ok ? "available" : "unavailable (cuda-gdb not in PATH)");
        return ok ? 0 : 1;
    }

    if (argc < 3) { Usage(argv[0]); return 2; }
    pid_t pid = (pid_t)atoi(argv[2]);
    if (pid <= 0) { fprintf(stderr, "invalid pid: %s\n", argv[2]); return 2; }
    if (kill(pid, 0) != 0) { perror("kill(pid,0)"); return 1; }

    auto &ctx = GdbInterruptContext::Instance();
    if (strcmp(cmd, "state") == 0) {
        printf("pid %d: %s\n", (int)pid, ReadAgent(pid) > 0 ? "suspended" : "running");
        return 0;
    }
    if (strcmp(cmd, "suspend") == 0) {
        pid_t holder = ReadAgent(pid);
        if (holder > 0) { printf("pid %d already suspended by agent %d\n", (int)pid, (int)holder); return 0; }
        if (!ctx.Suspend(pid)) { printf("suspend(%d) = FAILED\n", (int)pid); return 1; }
        FILE *f = fopen(AgentFile(pid).c_str(), "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
        printf("suspend(%d) = ok; holding. Ctrl-C (or `l3ctl resume %d`) to release.\n",
               (int)pid, (int)pid);
        fflush(stdout);
        signal(SIGINT, OnSignal);
        signal(SIGTERM, OnSignal);
        while (!g_stop) sleep(1);
        bool ok = ctx.Resume(pid);
        unlink(AgentFile(pid).c_str());
        printf("resumed(%d) = %s\n", (int)pid, ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(cmd, "resume") == 0) {
        pid_t holder = ReadAgent(pid);
        if (holder <= 0) { printf("pid %d is not suspended\n", (int)pid); return 0; }
        printf("asking agent %d to resume pid %d\n", (int)holder, (int)pid);
        if (kill(holder, SIGTERM) != 0) { perror("kill(agent)"); return 1; }
        for (int i = 0; i < 100 && ReadAgent(pid) > 0; i++) usleep(50000);
        // if the holder died without cleaning up, detach from here as a fallback
        if (ReadAgent(pid) > 0) {
            ctx.Resume(pid);
            unlink(AgentFile(pid).c_str());
        }
        return 0;
    }
    Usage(argv[0]);
    return 2;
}
