# Imgui-DX9-Software-Vertex-Processing-Repro

A repro for the catastrophic performance drop of ImGui's DX9 backend with software vertex processing (e.g. Mobile Intel(R) 945 Express Chipset).

The root cause is that the backend submits the entire vertex buffer to the transform stage, even if the draw command only uses a small range of it.

## Build and run

Dear ImGui is a submodule pinned at `9fb6f2d` (1.93.1 WIP):

```bat
git submodule update --init --recursive
build.cmd
dx9_vertex_range_bench.exe            :: software vertex processing (the failing case)
dx9_vertex_range_bench.exe --hwvp     :: hardware vertex processing (the control)
```

The device is created with `D3DCREATE_SOFTWARE_VERTEXPROCESSING`, so the bug is reproducible without old hardware.

## Repro

Run on an RTX 5090 (NVIDIA driver), 640x480, 120 frames per measurement, median, best of three.

Reproducer builds one Dear ImGui frame and then submits the very same
`ImDrawData` twice:

|              | `MinIndex` / `NumVertices`                                               |
| ------------ | ------------------------------------------------------------------------ |
| **upstream** | `0` / `list->VtxBuffer.Size`                                             |
| **patched**  | the lowest and highest index the command's own `ElemCount` entries reach |

Everything else is identical, so any timing difference is attributable to those two arguments alone.

### Software vertex processing

| scene                 | vtx    | cmds | declared upstream | declared exact | less work | upstream | exact   | speedup   |
| --------------------- | ------ | ---- | ----------------- | -------------- | --------- | -------- | ------- | --------- |
| menu chrome only      | 76     | 2    | 152               | 76             | 2.00x     | 0.02 ms  | 0.01 ms | 1.79x     |
| 1 row                 | 436    | 6    | 2 616             | 1 268          | 2.06x     | 0.35 ms  | 0.17 ms | 2.03x     |
| 5 rows                | 2 128  | 4    | 8 512             | 6 072          | 1.40x     | 1.13 ms  | 0.81 ms | 1.40x     |
| 20 rows               | 8 084  | 5    | 40 420            | 31 580         | 1.28x     | 5.38 ms  | 4.20 ms | 1.28x     |
| 40 rows               | 10 320 | 5    | 51 600            | 36 360         | 1.42x     | 6.87 ms  | 4.84 ms | 1.42x     |
| 100 rows              | 17 040 | 6    | 102 240           | 57 444         | 1.78x     | 13.70 ms | 7.67 ms | 1.78x     |
| 20 rows, +8 commands  | 8 108  | 13   | 105 404           | 31 884         | 3.31x     | 14.73 ms | 4.45 ms | 3.31x     |
| 20 rows, +24 commands | 8 176  | 29   | 237 104           | 31 956         | 7.42x     | 33.39 ms | 4.50 ms | **7.42x** |
| 20 rows, per-row clip | 8 080  | 25   | 202 000           | 31 976         | 6.32x     | 28.39 ms | 4.50 ms | **6.31x** |
| 40 rows, per-row clip | 10 320 | 25   | 258 000           | 36 476         | 7.07x     | 36.25 ms | 5.10 ms | **7.11x** |

### Hardware vertex processing

Both variants sit at 0.00-0.02 ms for every scene.

The only measurable cost the patch adds is the min/max scan itself: ~20 µs per frame at 17 000
vertices / 57 000 indices (`100 rows`, `exact` 0.02 ms vs `upstream` 0.00 ms).

## Reading the output

- `vtx` - `ImDrawData::TotalVtxCount` for the frame
- `cmds` - draw commands across all draw lists (one more per clip-rect change)
- `declared` - how many vertices each variant asks the transform stage to process
- `submit` - time inside the submission loop only (buffer upload + state + draw calls);
  `Present` and `Clear` are excluded, and Dear ImGui's widget code is not in the loop at all
- `speedup` - upstream submit ÷ exact submit
