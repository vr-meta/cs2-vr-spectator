# Third-party software in a release

A release zip contains this project's own files — `cs2vr.exe`, the VR parts of
`AfxHookSource2.dll`, the configs — and the following, which belong to other people. Their
licence texts ship inside `hook\`: `LICENSE-advancedfx.txt` in a zip built by CI, or a
whole `LICENSES\` folder when one is staged from a full HLAE install.

**Our licence does not reach any of it.** This project is [Apache 2.0](LICENSE), and that
governs our own code and nothing else: the advancedfx code compiled into
`AfxHookSource2.dll` is MIT and stays MIT in your hands, OpenEXR stays BSD-3-Clause, the
OpenXR loader stays Apache-2.0 under its own copyright. A licence on a combined work speaks
for the part its authors wrote, and cannot alter what somebody else already granted over
theirs — in either direction.

| Component | Where it is in the zip | Licence | What it is here for |
| --- | --- | --- | --- |
| [advancedfx / HLAE](https://github.com/advancedfx/advancedfx) | `hook\x64\AfxHookSource2.dll` (built from it, with this project's patches and sources added), `hook\resources\` | MIT | The hook itself: the render-pass loop, the engine hooks and the console this project builds on. |
| [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) | `hook\x64\openxr_loader.dll` | Apache-2.0 | Finds and loads the machine's OpenXR runtime. |
| [OpenEXR](https://github.com/AcademySoftwareFoundation/openexr) and [Imath](https://github.com/AcademySoftwareFoundation/Imath) | `hook\x64\OpenEXR*.dll`, `Iex*.dll`, `IlmThread*.dll`, `Imath*.dll` | BSD-3-Clause | Imported by HLAE's image writers. Loaded with the hook whether or not anything is recorded. |
| Microsoft Visual C++ runtime | `hook\x64\msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll` | Microsoft redistributable terms | Shipped beside the hook so nothing has to be installed first. |
| Libraries linked into the hook by advancedfx (Detours, protobuf, Abseil, libdeflate, easywsclient, RapidXML and others) | inside `AfxHookSource2.dll` | see `hook\LICENSES\` and `hook\LICENSES\advancedfx\THIRDPARTY.yml` | HLAE's own dependencies. |

**Nothing of Valve's is included** — no game binaries, maps, models, sounds or demo files —
and nothing here may be used to redistribute them. Counter-Strike 2 has to be installed
from Steam by whoever runs this.

If a component is missing from this table, that is a bug in the table: please open an issue.
