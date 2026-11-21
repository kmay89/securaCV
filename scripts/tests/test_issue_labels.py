"""Every label an issue form applies exists, because it is declared.

GitHub silently drops a label an issue form names when the repository has no
such label: the issue opens, unlabeled, and nothing says so. `.github/labels.yml`
was written after that happened to `idea` (the website's Ideas board read
"no ideas yet" for every idea anyone sent), and it declared `idea` and the
three status labels. Seven other forms and three website links went on naming
labels nobody had created: until 2026-11 every feedback note, build
submission, hardware report and conformance report opened unlabeled.

This suite holds:

- every label named by a form in `.github/ISSUE_TEMPLATE/` is declared in
  `.github/labels.yml` (the sync workflow creates what is declared);
- every label the website names (carried in
  `canary-local/devices/site_contract.json`, when the site states them) is
  declared too;
- every declared label is one GitHub will accept: a unique name, a six-digit
  hex color, a description of at most 100 characters (the API refuses a longer
  one, and the sync would fail on merge, after review);
- the three flows that used to rely on `issues/new?labels=<name>` have a form:
  a label in a URL is applied only for someone allowed to label issues here,
  which a visitor is not, while a form's own `labels:` apply for everyone.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import json
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
MANIFEST = REPO / ".github" / "labels.yml"
FORMS = REPO / ".github" / "ISSUE_TEMPLATE"
CONTRACT = REPO / "canary-local" / "devices" / "site_contract.json"

# A flow the website opens for a visitor, and the form that labels it.
VISITOR_FLOWS = {
    "maker_corps.yml": "maker-corps",
    "build_request.yml": "build-request",
    "industry_inquiry.yml": "industry-inquiry",
}


def declared() -> list[dict]:
    return yaml.safe_load(MANIFEST.read_text(encoding="utf-8"))["labels"]


def forms() -> dict[str, dict]:
    out = {}
    for path in sorted(FORMS.glob("*.yml")):
        if path.name == "config.yml":
            continue
        out[path.name] = yaml.safe_load(path.read_text(encoding="utf-8"))
    return out


class Declared(unittest.TestCase):
    def test_every_label_is_one_github_accepts(self):
        seen = set()
        for label in declared():
            name = label["name"]
            self.assertNotIn(name.lower(), seen, f"{name} is declared twice")
            seen.add(name.lower())
            self.assertRegex(str(label["color"]), r"^[0-9a-fA-F]{6}$", name)
            self.assertIsInstance(label["color"], str,
                                  f"{name}: quote the color, or YAML reads 000000 as a number")
            description = label.get("description", "")
            self.assertTrue(description, f"{name} has no description")
            self.assertLessEqual(len(description), 100,
                                 f"{name}: GitHub refuses a description over 100 characters")

    def test_every_form_label_is_declared(self):
        names = {label["name"] for label in declared()}
        found = forms()
        self.assertGreaterEqual(len(found), 10, sorted(found))
        for filename, form in found.items():
            labels = form.get("labels") or []
            self.assertTrue(labels, f"{filename} applies no label: it would land untriaged")
            for label in labels:
                self.assertIn(label, names,
                              f"{filename} applies {label!r}, which .github/labels.yml does not "
                              f"declare: GitHub would drop it silently")

    def test_every_label_the_website_names_is_declared(self):
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        labels = contract.get("labels")
        if labels is None:
            self.skipTest("the carried site contract states no labels yet")
        names = {label["name"] for label in declared()}
        for label in labels:
            self.assertIn(label, names, f"securacv.com names the label {label!r}")


class VisitorFlowsHaveForms(unittest.TestCase):
    def test_each_flow_has_a_form_that_applies_its_label(self):
        found = forms()
        for filename, label in VISITOR_FLOWS.items():
            self.assertIn(filename, found, f"{filename} is gone")
            self.assertEqual(found[filename].get("labels"), [label], filename)

    def test_each_form_takes_the_sites_prefill(self):
        # The site opens `issues/new?template=<form>&details=<text>`: a form
        # field is filled from the query by its id, so the id is a contract.
        for filename in VISITOR_FLOWS:
            body = forms()[filename]["body"]
            details = [field for field in body if field.get("id") == "details"]
            self.assertEqual(len(details), 1, filename)
            self.assertEqual(details[0]["type"], "textarea", filename)
            self.assertTrue(details[0].get("validations", {}).get("required"), filename)

    def test_each_form_says_it_is_public(self):
        for filename in VISITOR_FLOWS:
            text = (FORMS / filename).read_text(encoding="utf-8").lower()
            self.assertRegex(text, r"public|in the open", filename)

    def test_the_sync_runs_when_the_manifest_changes(self):
        workflow = (REPO / ".github" / "workflows" / "labels.yml").read_text(encoding="utf-8")
        self.assertIn(".github/labels.yml", workflow)
        self.assertIn("sync_labels.py", workflow)


if __name__ == "__main__":
    unittest.main()
