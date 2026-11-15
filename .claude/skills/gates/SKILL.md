---
name: gates
description: Run the CI gates the current diff trips, derived from the workflows' path filters and steps. Use before pushing, or to answer "what will CI run for this change".
---
From the repository root:

```sh
python3 scripts/gates.py --run        # the fast scripted gates, here
python3 scripts/gates.py --all        # plus the toolchain steps CI runs that cannot run here
python3 scripts/gates.py --paths a b  # for a hypothetical diff
```

Paste the summary line, and any FAIL block, into the PR body under
"Checks run". A FAIL is yours to fix before pushing; a manual step is CI's
to run, so say in the PR which ones apply.
