# Perk Conditions Framework

Perk Conditions Framework is an F4SE and CommonLibF4 patcher that allows mod authors and users to dynamically add, swap, expand, replace, or remove conditions used by crafting recipes, dialogue, message boxes, and more.

## Requirements

- [CMake](https://cmake.org/)
  - Add this to your `PATH`
- [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
- [Vcpkg](https://github.com/microsoft/vcpkg)
  - Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
- [Visual Studio Community 2026](https://visualstudio.microsoft.com/)
  - Desktop development with C++
- [CommonLibF4RD](https://github.com/Zzyxz/CommonLibF4RD)
  - Add this as an environment variable `CommonLibF4Path`
- [toml++](https://github.com/marzer/tomlplusplus)
  - Add this as an environment variable `TOMLPLUSPLUS_PATH`

## User Requirements

- [F4SE](https://f4se.silverlock.org/)
- [Runtime Database](https://www.nexusmods.com/fallout4/mods/108394)

## Register Visual Studio as a Generator

- Open `x64 Native Tools Command Prompt`
- Run `cmake`
- Close the cmd window

## Building

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
