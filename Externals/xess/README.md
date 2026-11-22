# Intel XeSS SDK 3.0.2 (XeSS Super Resolution 2.0.2, vendored subset)

XeSS Super Resolution for the three renderers: `libxess.dll` serves DirectX 12 (any GPU with Shader Model 6.4 and DP4a)
and Vulkan, `libxess_dx11.dll` DirectX 11 (Intel Arc only). The modules load them by their absolute path next to the
executable and take the entry points by name (`src/Layers/xrRenderPC_R4/da_xess.cpp`): without them XeSS is simply
not offered. Plain vendor drop of the release package, nothing modified. Frame generation (`libxess_fg.dll`) and low
latency (`libxell.dll`) are not taken.

| | |
| --- | --- |
| Version | XeSS SDK 3.0.2, release `v3.0.2` of https://github.com/intel/xess (`XeSS_SDK_3.0.2.zip`, SHA-256 `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`); both DLLs file version 2.0.2.68 |
| Signature | both DLLs: Authenticode, Intel Corporation |
| Licence | `LICENSE.txt` (Intel Simplified Software License: binary redistribution without modification), `third-party-programs.txt` |

## What was taken

| Here | Package path | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `bin/libxess.dll` | `bin/` | 77,795,704 | `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7` |
| `bin/libxess_dx11.dll` | `bin/` | 156,016 | `c7cfe86f0c9d94e4fb3696d3cd5035e2bbb6a8b1b0572f8b7395a4cdfd0c625e` |
| `include/xess/xess.h` | `inc/xess/` | 15,991 | `98dc435f5aed252d83a161fc1f86223f20c67eb863214fe3b5cb215a3f6e3319` |
| `include/xess/xess_d3d12.h` | `inc/xess/` | 8,107 | `a582a60c3e39ee6fd6aa4871e23082cfb5ec510f98a19fc04acf7a7edb2f6b6c` |
| `include/xess/xess_d3d11.h` | `inc/xess/` | 5,289 | `f154e48039ee4de4cb1ab94c908eeacfcbcd72ce46c5a44a57ad972b7907d1bb` |
| `include/xess/xess_vk.h` | `inc/xess/` | 12,482 | `859086df3c531ceccab628b287f69bd9a4528045bcbdd8c639589082dcbf8977` |
| `LICENSE.txt` | root | 4,194 | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |
| `third-party-programs.txt` | root | 35,788 | `e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c` |

The headers are used with `XESS_TYPES_ONLY` (types, no imports): nothing links against the import libraries.
