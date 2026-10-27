"""Claude skill shims: every canonical skill under .agents/skills/ is discoverable from .claude/skills/.

Claude Code loads project skills from .claude/skills/, while this repository keeps the canonical skill files
directly under .agents/skills/ (owner decision 2026-10-27: Claude Code writes here again). Each shim carries the
canonical frontmatter so discovery shows the same name and trigger text, and its body points at the one source of
truth. Stdlib only.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
AGENTS = REPO / ".agents" / "skills"
CLAUDE = REPO / ".claude" / "skills"


def frontmatter(path):
    """The name/description frontmatter of a SKILL.md file (single-line values)."""
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "---":
        return {}
    out = {}
    for line in lines[1:]:
        if line == "---":
            break
        if ": " in line:
            key, value = line.split(": ", 1)
            out.setdefault(key, value)
    return out


def canonical_skills():
    return sorted(p.name for p in AGENTS.iterdir() if (p / "SKILL.md").is_file())


class Shims(unittest.TestCase):
    def test_every_canonical_skill_has_a_matching_shim(self):
        skills = canonical_skills()
        self.assertTrue(skills, "no canonical skills found under .agents/skills/")
        for name in skills:
            with self.subTest(name):
                canon = frontmatter(AGENTS / name / "SKILL.md")
                self.assertEqual(canon.get("name"), name, "canonical frontmatter names the wrong skill")
                self.assertTrue(canon.get("description"), "canonical skill has no description")
                shim_path = CLAUDE / name / "SKILL.md"
                self.assertTrue(shim_path.is_file(), f"missing Claude shim for {name}")
                shim = frontmatter(shim_path)
                self.assertEqual(shim.get("name"), name, "shim frontmatter names the wrong skill")
                self.assertEqual(shim.get("description"), canon["description"], "shim description drifted")
                self.assertIn(f".agents/skills/{name}/SKILL.md", shim_path.read_text(encoding="utf-8"),
                              "shim body does not point at the canonical file")

    def test_no_orphan_shims(self):
        skills = set(canonical_skills())
        shims = sorted(p.name for p in CLAUDE.iterdir() if (p / "SKILL.md").is_file()) if CLAUDE.is_dir() else []
        for name in shims:
            with self.subTest(name):
                self.assertIn(name, skills, "shim without a canonical skill")


if __name__ == "__main__":
    unittest.main()
