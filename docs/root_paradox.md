# Host Compromise (“Root Paradox”)

The Privacy Witness Kernel **cannot** protect against a compromised host.
If an attacker controls the operating system (root/admin), the hardware, or the
runtime environment, they can:

- Read or alter process memory, including secrets in-use.
- Bypass the kernel’s enforcement paths and modify binaries or dependencies.
- Access vault files or databases directly, regardless of policy.
- Forge or delete logs and receipts outside the kernel’s control.

This is a **fundamental limitation**: the kernel’s guarantees are architectural
*within a trusted host boundary*. Once that boundary is breached, guarantees no
longer hold.

## Implications

- The kernel’s invariants are **not** a defense against a hostile host.
- Break-glass quorum, contract enforcement, and retention rules assume the
  operating system and storage are trusted.
- Any “rooted” deployment must be treated as **fully compromised** and its
  outputs as untrustworthy.

## Out-of-Scope Mitigations

Hardening the host, securing physical access, and using hardware security modules
are essential operational controls, but they are **outside** the kernel’s scope.
They must be provided by the deployment environment, not by the kernel.

This limitation is explicit to prevent accidental capability expansion and to
ensure deployments are honest about what the kernel cannot defend.
