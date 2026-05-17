# FTXUI Hello World

Standalone CMake project for testing the repository's current FTXUI dependency.

Configure and build from the repository root:

```powershell
cmake -S examples/ftxui_helloworld -B build/ftxui_helloworld `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DVCPKG_MANIFEST_DIR=C:/Users/shao/acecode `
  -DVCPKG_INSTALLED_DIR=C:/Users/shao/acecode/build/vcpkg_installed

cmake --build build/ftxui_helloworld --config Debug
```

Run:

```powershell
.\build\ftxui_helloworld\Debug\helloworld.exe
```

Press `q` or `Esc` to exit.
