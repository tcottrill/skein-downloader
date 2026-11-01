# Language selector flags

The 14 flag icons are checked-in PNGs embedded into the application at build time.
Each icon has 20, 30, 40, and 60 pixel sizes for DPI-aware rendering. `system`
is the globe shown for **Same as system**. These images were exported from the
application's original procedural flag drawings; they use no downloaded artwork.

`CMakeLists.txt` embeds these files in `language_flags.h`. The selector decodes
them once when it is created and reuses the cached bitmap bundles for its menu.
