# Third-party components

BlendNet is distributed under the GPL v3 (see LICENSE). Blender is downloaded
separately and is not included in the application packages.

| Component | License | Source |
| --- | --- | --- |
| Asio standalone | Boost Software License 1.0 | https://think-async.com/Asio/ |
| OpenSSL | Apache 2.0 (3.x); OpenSSL/SSLeay (1.1.1) | https://openssl.org/ |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| libzip | BSD 3-Clause | https://libzip.org/ |
| zlib | zlib | https://zlib.net/ |
| libcurl | curl license | https://curl.se/ |
| reproc | MIT | https://github.com/DaanDeMeyer/reproc |
| GLFW | zlib/libpng | https://www.glfw.org/ |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| DejaVu Sans | Bitstream Vera; DejaVu changes public domain | https://dejavu-fonts.github.io/ |

The font and its license are included in this repository. Packages include the
available upstream notices from FetchContent and vcpkg. Native Linux packages
also depend on system libraries, whose notices are installed by their packages.
AppImages may contain additional dependencies gathered by linuxdeploy; retain their
notices alongside the image when redistributing it.

Source releases can be generated with CPackSourceConfig.cmake. Exact versions of
fetched libraries are recorded in CMakeLists.txt; Windows dependency versions are
pinned by vcpkg.json. System dependency versions depend on the build environment.
