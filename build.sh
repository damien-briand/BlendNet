#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
build_dir="$project_dir/build"

configure_args=(-S "$project_dir" -B "$build_dir" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug)

# Un cache CMake ne peut pas changer de générateur. Préserver l'ancienne compilation
# et ses dépendances avant de recréer build/ avec Unix Makefiles.
previous_generator=""
if [[ -f "$build_dir/CMakeCache.txt" ]]; then
    while IFS= read -r line; do
        if [[ "$line" == CMAKE_GENERATOR:INTERNAL=* ]]; then
            previous_generator="${line#*=}"
            break
        fi
    done < "$build_dir/CMakeCache.txt"
fi

if [[ -n "$previous_generator" && "$previous_generator" != "Unix Makefiles" ]]; then
    backup_dir="$(mktemp -d "$project_dir/build-backup-XXXXXX")"
    mv -- "$build_dir" "$backup_dir/build"
    printf 'Ancienne compilation (%s) sauvegardée dans %s/build\n' "$previous_generator" "$backup_dir"

    # Les sources FetchContent sont indépendantes du générateur ; les sous-builds
    # et leurs caches restent dans la sauvegarde et ne sont pas réutilisés.
    for dependency in JSON:json IMGUI:imgui GLFW:glfw GOOGLETEST:googletest; do
        dependency_name="${dependency%%:*}"
        dependency_source="$backup_dir/build/_deps/${dependency#*:}-src"
        if [[ -f "$dependency_source/CMakeLists.txt" ]] ||
            [[ "$dependency_name" == IMGUI && -f "$dependency_source/imgui.cpp" ]]; then
            configure_args+=("-DFETCHCONTENT_SOURCE_DIR_$dependency_name=$dependency_source")
        fi
    done
fi

# Supprimer tout ce qui se trouve dans le /build sauf les libblendnet
find "$build_dir" ! -name "libblendnet*" -mindepth 1 -maxdepth 1 -exec rm -rf {} +

cmake "${configure_args[@]}" "$@"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "$build_dir" --output-on-failure