# Kin gameplay test runner

`kin_test.py` runs JSON/Lua scenarios, recording each driven scene-server
session, and replays JSONL recordings (its own or a game's `--automation-record`). It uses Python 3.9+ without third-party packages.

```sh
python3 tools/automation/kin_test.py run examples/2d_workflow/tests/workshop.json \
  --out /tmp/workshop-test --game build/bin/kin_2d_workflow
python3 tools/automation/kin_test.py replay /tmp/workshop-test/commands.jsonl \
  --out /tmp/workshop-replay --game build/bin/kin_2d_workflow
```

See [the automation guide](../../docs/automation.md) for conditions, Lua, scoped
selectors, dependency identities, recording boundaries, failure artifacts and CI.
The reusable Python API is `kintest.Client` / `kintest.Runner`.
