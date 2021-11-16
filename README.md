# Custom for OBS Studio

Plugin for OBS Studio to add a custom delay using a filter

# Build (Linux)
- Clone OBS Studio
- `cd obs-studio`
- Clone this repository to plugins folder
- Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
- Add `add_subdirectory(obs-custom-delay)` to plugins/CMakeLists.txt
- Rebuild OBS Studio

