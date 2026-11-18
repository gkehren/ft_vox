#!/usr/bin/env python3
"""GPU workflow trust-policy guard (PR #244 review correction).

Stdlib-only structural checks on .github/workflows/gpu-validation.yml — the
security contract is asserted on every CPU CI run so a regression (a PR
trigger creeping back, a widened ref condition, persisted credentials,
privileged installation, secret exposure) fails loudly. Complements the
YAML validity lint; deliberately fail-closed on structural drift.

Checks (Option A trust model):
  1. No pull_request / pull_request_target trigger (untrusted PR code must
     never reach the persistent self-hosted GPU runner).
  2. workflow_dispatch is the only trigger.
  3. The GPU job condition pins the protected ref: github.ref ==
     'refs/heads/main' (a dispatch on any other ref skips the job).
  4. Minimal permissions: contents: read (top-level).
  5. Checkout does not persist credentials.
  6. No privileged package installation (sudo) in any run step.
  7. No secrets.* reference anywhere in the workflow.
  8. cpu-boundary.yml keeps its per-PR pull_request gate (unaffected).

Usage: python3 tests/check_gpu_workflow.py [--repo-root DIR]
Exit 0 on pass, 1 on any violation (messages name the violated invariant).
"""

import argparse
import pathlib
import re
import sys


def fail(violations, message):
    violations.append(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args()
    root = pathlib.Path(args.repo_root) if args.repo_root else pathlib.Path(__file__).resolve().parent.parent
    wf_dir = root / ".github" / "workflows"
    gpu_path = wf_dir / "gpu-validation.yml"
    cpu_path = wf_dir / "cpu-boundary.yml"
    violations = []

    if not gpu_path.is_file():
        print(f"FAIL: missing {gpu_path}")
        return 1
    gpu = gpu_path.read_text(encoding="utf-8")

    # 1+2. Trigger surface: workflow_dispatch only, never PR events.
    if re.search(r"^\s*pull_request(_target)?\s*:", gpu, re.MULTILINE):
        fail(violations, "gpu-validation.yml must not trigger on pull_request events (untrusted PR code on a persistent runner)")
    if not re.search(r"^\s*workflow_dispatch\s*:", gpu, re.MULTILINE):
        fail(violations, "gpu-validation.yml must be workflow_dispatch-only")
    if re.search(r"^\s*(push|schedule|repository_dispatch)\s*:", gpu, re.MULTILINE):
        fail(violations, "gpu-validation.yml must not add push/schedule/repository_dispatch triggers")

    # 3. The GPU job pins the protected main ref in its condition.
    if "github.ref == 'refs/heads/main'" not in gpu:
        fail(violations, "the GPU job condition must pin github.ref == 'refs/heads/main'")
    if "self-hosted" not in gpu:
        fail(violations, "expected the self-hosted GPU runner declaration")

    # 4. Minimal top-level permissions.
    if not re.search(r"^permissions:\s*\n\s+contents:\s*read\s*$", gpu, re.MULTILINE):
        fail(violations, "top-level permissions must be exactly 'contents: read'")
    if re.search(r"^\s+(contents|actions|pull-requests|statuses|deployments|packages|security-events|checks):\s*write", gpu, re.MULTILINE):
        fail(violations, "no write permission may be granted")

    # 5. Checkout credentials not persisted.
    if "persist-credentials: false" not in gpu:
        fail(violations, "actions/checkout must set persist-credentials: false")

    # 6. No privileged installation in run steps.
    if re.search(r"\bsudo\b", gpu):
        fail(violations, "no sudo/privileged package installation in the GPU workflow")

    # 7. No secrets exposure.
    if re.search(r"secrets\.", gpu):
        fail(violations, "the GPU workflow must not reference secrets.*")

    # 8. CPU CI keeps its per-PR gate.
    if not cpu_path.is_file():
        fail(violations, "cpu-boundary.yml missing (the per-PR CPU gate must remain)")
    else:
        cpu = cpu_path.read_text(encoding="utf-8")
        if not re.search(r"^\s*pull_request\s*:", cpu, re.MULTILINE):
            fail(violations, "cpu-boundary.yml must keep its pull_request trigger")

    if violations:
        for v in violations:
            print(f"FAIL: {v}")
        return 1
    print("GpuWorkflowTrustGuard: PASS (dispatch-only, main-ref trust, minimal permissions, no credentials/sudo/secrets; CPU gate intact)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
