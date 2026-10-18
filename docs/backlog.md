# Development backlog

On 2026-10-18, the user deferred the remaining 05c work and authorized continued
development, including 06b and later increments. These items are **deferred and
nonblocking**, not fixed or passed. Full `REQ-05` remains **NOT_RUN**; historical
failed receipts, fixed resource budgets and both current-PC GPU targets remain
unchanged. This decision supersedes the earlier CPU-only 06a sequencing exception
and the progression restrictions in historical handoffs.

Do not repeat these known items in routine progress updates, handoffs or final
responses. Keep this backlog as their current status record. Revisit an item when
its trigger occurs, when new evidence materially changes its impact, or when the
user asks. A new regression in current work still requires investigation; it must
not be dismissed solely because it resembles a deferred issue.

| ID | Deferred work and retained evidence | Revisit trigger / completion evidence |
| --- | --- | --- |
| BL-05C-01 | NVIDIA 610.62 retains a desktop DC per swapchain lifetime, independently reproduced outside Shark. Driver acquisition is identified; no supported engine correction is established. [Investigation](handoffs/05c-failure-investigation.md), [receipt](coverage/evidence/05c-investigation.json). | An independently authorized driver/OS change, a supported ownership correction, or material impact during normal development. Rerun the independent reproducer and the unchanged qualifying workload; retain old failures. |
| BL-05C-02 | Intel 101.6790 retains USER objects in raw Win32/Vulkan lifetimes. Tracing identifies failed driver-owned hook removal after target threads begin exit. [Investigation](handoffs/05c-intel-asan-investigation.md), [receipt](coverage/evidence/05c-intel-user-investigation.json). | An independently authorized driver/OS change, a supported driver-lifetime correction, or material impact during normal development. Repeat the independent reproduction and qualifying workload; require bounded resource behavior. |
| BL-05C-03 | An original Intel ASan fault child exceeded its exit deadline after native cleanup. The fixed 64-child diagnostic sequence did not reproduce or explain it. The separate queue-worker deadline defect is already fixed. [Investigation](handoffs/05c-intel-asan-investigation.md), [receipt](coverage/evidence/05c-asan-exit-investigation.json). | Another equivalent timeout, actionable process/thread capture, or a relevant shutdown change. Use the existing bounded capture tools to identify the stalled process and demonstrate a targeted correction; successful retries alone do not close the original finding. |
| BL-05C-04 | Remaining full Release/ASan stress qualification on both GPUs, including the sustained workload and repeated lifetimes. Earlier partial and failed runs remain intact. [Qualification contract](presentation-qualification.md), [original handoff](handoffs/05c-presentation-qualification.md), [latest handoff](handoffs/05c-intel-asan-investigation.md). | Resume when the underlying findings have an actionable resolution or when the user requests qualification. Execute the existing workloads and budgets, Windows/WSL regressions and applicable package checks. Only complete qualifying evidence can change full `REQ-05` to PASS. |

This deferral does not authorize driver installation, host resource changes,
foreign-handle cleanup or relaxed acceptance limits. The active implementation
sequence is recorded in the [06 checkpoint plan](roadmap/shader-resource-plan.md).
