#pragma once

// Shared control block between an XSched-managed CUDA process (target) and an
// external sm120 trap agent.  The target publishes its RM handles, the agent
// publishes its own client plus the debugger session it built, and the two
// sides handshake over trigger/resume requests.
//
// Both sides must be rebuilt together if this layout changes.

#include <cstdint>

#define L3_TRAP_SHM_NAME  "/xsched-sm120-trap"
#define L3_TRAP_SHM_MAGIC 0x4c3354525f534d31ULL   // "L3TR_SM1"
#define L3_TRAP_MAX_KGO   16

struct L3TrapShm
{
    volatile uint64_t magic;        // target -> agent: handles are valid
    uint32_t pid;                   // target pid
    uint32_t app_client;            // target RM client
    uint32_t device;                // target device
    uint32_t subdev;                // target subdevice
    uint32_t app_session;           // target's own 0x83de session (informational)
    uint32_t ncompute;              // number of KGOs below
    uint32_t computes[L3_TRAP_MAX_KGO];

    uint32_t agent_client;          // agent RM client
    uint32_t agent_device;          // agent device (session parent)
    uint32_t agent_subdev;
    volatile uint32_t agent_ready;  // agent -> target: client published

    volatile int32_t  policy_status[3];
    uint32_t session;               // agent's 0x83de session handle
    volatile uint32_t session_ready;
    volatile int32_t  session_status;

    // The 0x83de session may only exist while a preemption is in flight:
    // every kernel launch issued while it exists stops on a debugger event
    // that nothing answers.  The target therefore asks the agent to create
    // the session just before the 0317 trap and free it right after.
    volatile uint32_t session_req;       // target -> agent: create (generation)
    volatile uint32_t session_free_req;  // target -> agent: free the session
    volatile uint32_t session_free_ack;

    volatile uint32_t trigger_req;  // target -> agent: send 0317{0,0}
    volatile uint32_t trigger_ack;
    volatile int32_t  trigger_status;
    volatile uint32_t resume_req;   // target -> agent: send 0318
    volatile uint32_t resume_ack;
    volatile int32_t  resume_status;

    // diagnostics
    volatile uint32_t arm_count;
    volatile uint32_t trap_patch_ok;
};
