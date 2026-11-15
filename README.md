**SH4 DLL Loader**
A mod loader for Silent Hill 4: The Room (PC), built as a dinput8.dll proxy.

Works with the GOG version (v7). Support for other versions is planned.

**What it does**

Loads other mods before the game starts, such as UFO Ending, Restore and Recreate Unused Content, SkipIntro, FreeCamera, etc.  
Passes DirectInput through to dinput8_original.dll (the game's own dinput8.dll, renamed). If that file is missing, it uses the Windows dinput8.dll.  
Writes a log to SH4CustomModules.log, next to the game's exe, shared with the mods.

**Installation**

Rename the game's dinput8.dll to dinput8_original.dll. The release already includes this file.  
Copy this dinput8.dll to the game folder.  
Put the mod DLLs in the same folder.  
How mods are loaded  
A mod is any 32-bit DLL in the game folder that exports Initialize.  
.NET DLLs load first, then the rest in alphabetical order.  
Each Initialize() must finish before the game starts, so the mods' hooks are in place first.  
If a mod's Initialize() crashes, the loader shows an error and closes the game.  

**Building**  
Visual Studio, Release | Win32.
