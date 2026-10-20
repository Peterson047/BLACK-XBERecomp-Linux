# Decision Log

| Timestamp | Topic | Selection | Rationale |
|-----------|-------|-----------|-----------|
| 2026-10-17T19:50:00-03:00 | Graphics Backend | Dedicated Vulkan (`gpu::Backend`) | vkd3d lacks runtime HLSL compiler and Win32 swapchain support on native Linux. Vulkan with shaderc provides native high-performance presentation. |
| 2026-10-17T20:00:00-03:00 | Window & Input | SDL3 | SDL3 handles Wayland/X11 windowing, Vulkan surfaces, audio, mouse capture, and keyboard cleanly. |
| 2026-10-17T20:30:00-03:00 | Shader Compilation | `shaderc` runtime compilation | Xbox dynamically generates HLSL strings; shaderc compiles HLSL directly to SPIR-V in memory with deterministic register bindings. |
| 2026-10-17T20:36:00-03:00 | Binary Resource Embedding | Inline ASM `.incbin` in `europe_patch.cpp` | Replaces Windows resource compiler `.rc` with cross-platform native gas assembly directive directly in C++ translation unit. |
| 2026-10-17T20:46:00-03:00 | Descriptor Management | Fixed `std::array` for image/sampler infos | Prevents pointer invalidation and dangling references during `vkUpdateDescriptorSets`. |
| 2026-10-19 | Multiplayer launch retry | Keep the connection-triggered launch pending through startup media until the front-end state; discard only if a world is already active | Runtime log showed the original hook dropped the request during startup, then the game followed the campaign path. |
| 2026-10-19 | SDL gamepad event thread | Handle add/remove events from the Linux main event loop and do not call `SDL_PumpEvents` from guest input polling | SDL assertion explicitly required pumping on the main thread. |

| 2026-10-20 | Loading crash 0x151408 | Preserve original speech.slb instead of generating a zero-entry header | Restoring the ISO's 155-entry table eliminated the reproduced null lookup; muting playback does not remove the loader's table requirement. Package verification now rejects the empty substitute. |
| 2026-10-20 | Direct launch crash 0x1F5F0B | Bind initialized port 0 controller and the native action-map pointer before transition | The skipped PressStart path performs these bindings. Re-evaluation corrected the earlier mistaken interpretation of controller records as saved profiles. No save-profile construction was added. |
| 2026-10-20 | Runtime validation | Build, real host load, screenshot, UDP JoinAck/Pong | Host reached visible gameplay and accepted a temporary probe client; 45-second prior run survived. Left the subsequent host open for user testing. Two-client gameplay remains unverified. |

| 2026-10-20 | Native puppet safety | Remove guessed actor fallback and vtable replacement; never raise native actor count on client join | Client presence does not construct guest actors. Only registered actors with expected identity may be updated; dedicated safe remote-model allocation remains future work. Final host passed state/ping/disconnect/rejoin checks while continuing to render. |

| 2026-10-20 | Admin debug | Host role grants F8 access; mutations gated in NetworkManager | Clients cannot invoke host admin operations through the UI/API. |
| 2026-10-20 | Actor container correction | Preserve capacity/used and native category; remove 9999 teleport and category-zero suppression | Native collision and component dispatch crashed with those inferred enable/hide writes. Campaign AI removal requires a supported engine path. |
| 2026-10-20 | Shot replication | Confirm local shots from native magazine decrement | Render-frame fire-request flags can already be cleared; confirmed ammo consumption avoids missed or phantom shot packets. |
