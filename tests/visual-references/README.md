# Visual regression references

Committed golden references for `ft_vox_visual_tests` (ctest:
`VisualRegression`). One `<scene>.png` per scene, 640×360, rendered through
the production pass graph on the canonical developer GPU (NVIDIA GeForce
RTX 4070 Ti, Windows, Vulkan).

Regenerate **only** via the harness — normal test runs never write here:

```bash
./build/tests/Release/ft_vox_visual_tests.exe --update-references   # repo root
```

A reference update must go through a PR that includes and reviews the PNG
diff. See [`docs/visual-regression.md`](../../docs/visual-regression.md) for
scenes, tolerances, and policies.
