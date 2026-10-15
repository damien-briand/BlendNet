# BlendNet
Rendu Blender distribué entre pairs (C++20 / Qt 6 QML). Licence GPL v3+.

## Build (P1)
Prérequis : Qt ≥ 6.5 (Core, Network, Concurrent, Gui, Qml, Quick), CMake ≥ 3.21, Ninja, un compilateur C++20.

**Avec vcpkg** (recommandé, requis pour la CI Windows/macOS) :
```
export VCPKG_ROOT=/chemin/vers/vcpkg
# Remplacer "builtin-baseline" dans vcpkg.json par : git -C $VCPKG_ROOT rev-parse HEAD
cmake --preset vcpkg && cmake --build --preset vcpkg && ctest --preset vcpkg
```
**Sans vcpkg** (GoogleTest système) : `cmake --preset system && cmake --build --preset system`.

Qt n'est PAS dans vcpkg.json (installeur officiel ou paquets système) ; indiquer son emplacement via `CMAKE_PREFIX_PATH`.
