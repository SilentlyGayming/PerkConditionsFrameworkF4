# Perk Conditions Framework

Perk Conditions Framework is an F4SE and CommonLibF4 patcher that allows mod authors and users to dynamically add, swap, expand, replace, or remove conditions used by crafting recipes, dialogue, message boxes, and more.

## Requirements

* [CMake](https://cmake.org/)

  * Version 4.3 or newer; add this to your `PATH`
* [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
* [Vcpkg](https://github.com/microsoft/vcpkg)

  * Add the environment variable `VCPKG\_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio Community 2026](https://visualstudio.microsoft.com/)

  * Desktop development with C++, x64 MSVC v145, and Windows SDK
* [CommonLibF4 AV](https://github.com/LucaDotGit/CommonLibF4)

  * Add this as an environment variable `CommonLibF4Path`
* [toml++](https://github.com/marzer/tomlplusplus)

  * Add this as an environment variable `TOMLPLUSPLUS\_PATH`

## User Requirements

* [F4SE](https://f4se.silverlock.org/)
* [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)

## Register Visual Studio as a Generator

* Open `x64 Native Tools Command Prompt`
* Run `cmake`
* Close the cmd window

## Building

Double-click `build.bat` to prepare the pinned dependencies and build the Release DLL. This archive contains source only.

```bat
git clone https://github.com/SilentlyGayming/PerkConditionsFrameworkF4.git
cd PerkConditionsFrameworkF4
```

```bat
cmake --preset vs2026-windows-vcpkg
cmake --build --preset vs2026-release
```

## License

[MIT](LICENSE)
