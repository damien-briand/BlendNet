cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure