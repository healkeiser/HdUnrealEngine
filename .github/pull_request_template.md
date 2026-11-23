<!--
Title: one plain sentence saying what now happens, in the present tense, for example
  Debug views are opaque, as UE's viewport shows them
It becomes the squash commit. See CONTRIBUTING.md.
-->

## What this changes

<!-- What was wrong or missing, and what the user will see now. For a translation choice, cite the UE source or the spec. -->

## How it was verified

<!-- Tick what you ran. The first two need neither UE nor Houdini. -->

- [ ] Formatting and lint (Black, Ruff, clang-format, PSScriptAnalyzer)
- [ ] `.\scripts\Test.ps1 -Suite unit`
- [ ] Backend and Houdini delegate built
- [ ] `run_conformance.py --only <area>/*` for the areas touched: <!-- areas, failing runs -->
- [ ] Cornell husk render compared with the previous build
- [ ] Generated files regenerated with their script, not by hand

Closes #
