# WoundMeshes v0.1 (Visual Studio version)

An F4SE plugin. When a bullet hits an actor, it attaches `meshes\gore\bulletwoundsmall.nif` at the impact point, parented to the bone that was hit, so it follows animation and ragdoll.

It targets the current Steam version (1.11.x). Everything it needs is included; there's nothing else to download.

## Get the DLL

1. **Unzip** this folder somewhere with a short path, for example `C:\Mods\WoundMeshes`.
2. Open **Visual Studio** and click **Open a folder**. Select the `WoundMeshes` folder: the one containing `CMakeLists.txt`.
3. Wait for setup to finish. The **Output** window at the bottom shows progress and ends with "CMake generation finished".
   - Make sure the dropdown in the top toolbar says **Release (use this)**.
4. From the menu, click **Build → Build All**. The first build compiles the bundled library, which takes a few minutes. Later builds are fast.
5. When the Output window says the build succeeded, open the **`output`** folder inside `WoundMeshes`. `WoundMeshes.dll` is there.
6. Copy `WoundMeshes.dll` to `Fallout 4\Data\F4SE\Plugins\`.

If the build fails, open **View → Error List**, or copy the text from the Output window, and paste it to Claude.

**If "Build All" is greyed out or there's no CMake option:** run the **Visual Studio Installer**, click **Modify**, and make sure **Desktop development with C++** is ticked. Its "C++ CMake tools for Windows" part must be included; it is by default.

## In-game requirements

- **F4SE** for your game version.
- **Address Library for F4SE Plugins** (Nexus).
- `meshes\gore\bulletwoundsmall.nif`, plus its textures.

## Test

1. Load a save and shoot an NPC. Melee hits and the player are skipped.
2. Open the log at `Documents\My Games\Fallout4\F4SE\WoundMeshes.log`. You should see:
   - `Loaded wound model ...`
   - `Hit event sink registered`
   - `Wound on ...` for each hit
3. Look at the wound up close. Use `tfc` in the console to fly the camera.

## Tuning

Settings are at the top of `src\main.cpp`, in `namespace Config`. Change them, then **Build → Build All** again.

| What you see | Setting to change |
|---|---|
| Wound lying sideways or facing into the body | `kTransposeRotation = true` |
| Wound floating above the skin, or buried | `kSinkDepth` |
| Wound the wrong size | `kWoundScale` |
| Wound angled oddly | `kUseImpactNormal = false` |
| You want wounds on the player too | `kAffectPlayer = true` |

## How the NIF should be set up

- **Origin:** the wound centre, at skin level.
- **Facing:** the wound faces **+Z**, out of the skin.
- **Size:** game units. One unit is about 1.4 cm.
- **Mesh:** an unskinned `BSTriShape`.
- **Material:** `BSLightingShaderProperty` with `_d` / `_n` / `_s` textures.
- **Shader flags:** set the decal flags.
- **Collision:** none.

## Not in v0.1

- Saving wounds across save/load.
- Bending wounds with joints.
- Turning off the vanilla blood decal.
