"""The agent configuration under .claude/ is well-formed and points at real files.

.claude/settings.json is read by Claude Code at session start: a hook whose
script moved refuses every Read silently, a deny pattern that is not a string
is ignored, and nothing in CI noticed either until this test. Pinned: the
settings parse; every deny entry is a `Tool(pattern)` string; every hook
command names a script that exists in the tree; every .claude/rules file with
a `paths:` scope lists strings; every skill has a name and a description.
Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
import json
import re
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
SETTINGS = REPO / ".claude" / "settings.json"
DENY_RE = re.compile(r"^(Read|Edit|Write|Bash|WebFetch)\(.+\)$")
SCRIPT_RE = re.compile(r"\$CLAUDE_PROJECT_DIR/([^\s\"']+)")


def frontmatter(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---"):
        return {}
    end = text.find("\n---", 3)
    return yaml.safe_load(text[3:end]) or {} if end != -1 else {}


class ClaudeSettings(unittest.TestCase):
    def setUp(self):
        self.settings = json.loads(SETTINGS.read_text(encoding="utf-8"))

    def test_every_deny_entry_is_a_tool_pattern(self):
        deny = self.settings.get("permissions", {}).get("deny", [])
        self.assertTrue(deny, "the deny list is empty")
        for entry in deny:
            self.assertIsInstance(entry, str, entry)
            self.assertRegex(entry, DENY_RE)

    def test_every_hook_names_a_script_in_the_tree(self):
        hooks = self.settings.get("hooks", {})
        self.assertTrue(hooks, "no hooks configured")
        seen = 0
        for event, matchers in hooks.items():
            for m in matchers:
                for h in m.get("hooks", []):
                    self.assertEqual(h.get("type"), "command", (event, h))
                    for rel in SCRIPT_RE.findall(h["command"]):
                        self.assertTrue((REPO / rel).is_file(), f"{event}: {rel} does not exist")
                        seen += 1
        self.assertGreater(seen, 0, "no hook command names a $CLAUDE_PROJECT_DIR script")

    def test_rules_scopes_and_skills_are_well_formed(self):
        rules = sorted((REPO / ".claude" / "rules").glob("*.md"))
        self.assertTrue(rules)
        for rule in rules:
            paths = frontmatter(rule).get("paths")
            if paths is not None:
                self.assertIsInstance(paths, list, rule.name)
                self.assertTrue(all(isinstance(p, str) and p for p in paths), rule.name)
        for skill in sorted((REPO / ".claude" / "skills").glob("*/SKILL.md")):
            fm = frontmatter(skill)
            self.assertEqual(fm.get("name"), skill.parent.name, skill)
            self.assertTrue(fm.get("description"), skill)


if __name__ == "__main__":
    unittest.main()
