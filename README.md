# Perk Conditions Framework

Perk Conditions Framework is an F4SE and CommonLibF4 patcher that allows mod authors and users to dynamically add, swap, expand, replace, or remove conditions used by crafting recipes, dialogue, message boxes, and more.

## Build Requirements

* [CMake](https://cmake.org/) 4.3 or newer
* [Git](https://git-scm.com/)
* [Visual Studio Community 2026](https://visualstudio.microsoft.com/) with Desktop development with C++, MSVC v145 (x64), and Windows SDK

## User Requirements

* [F4SE](https://f4se.silverlock.org/)
* [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)

## Building

Run from the repository root:

```bat
cmake --preset vs2026-windows-vcpkg
cmake --build --preset vs2026-release
```

Dependencies are downloaded automatically on first configure. Output: `build/Release/PerkConditionsFramework.dll`.

## License

[MIT](LICENSE)
