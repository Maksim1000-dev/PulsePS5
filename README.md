# KytyPS5-Plus

**KytyPS5-Plus** is a custom enhanced fork of the experimental PlayStation 5 emulator [KytyPS5](https://github.com), focused on shader recompiler workarounds, improved runtime stability, and better playability in **Grand Theft Auto V**, **Astro Bot**, **Astro's Playroom**, **Minecraft Dungeons II**, and more PS5 titles.

## ✨ Key Improvements & Fixes

* **Safe Sampler Descriptor Fallback (`ResourceTracking.cpp`):**
  * Fixed the fatal `GetSamplerResource dword 3 is not a valid runtime value` crash in **Grand Theft Auto V (PS5)** when rotating the camera toward the horizon/sky during story missions.
  * Instead of triggering `std::abort()`, invalid runtime `dword 3` sampler values safely fall back to a default `0u` border clamp descriptor.
* **Built for Heavy & Popular PS5 Titles:**
  * Tailored and tested for smoother sessions in **GTA V**, **Astro Bot**, **Astro's Playroom**, **Hollow Knight: Silksong**, **Minecraft Dungeons II**, and other titles.

## ⚠️ Notes

* Based on the upstream [KytyPS5]([https://github.com/K](https://github.com/KytyPS5/KytyPS5)) project. Intended strictly for research and running legally owned game backups.
* After replacing `KytyPS5.exe` with a patched build, it is recommended to clear your old `shader_cache` folder so affected shaders recompile cleanly.
