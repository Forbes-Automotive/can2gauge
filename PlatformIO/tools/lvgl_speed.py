"""
lvgl_speed.py - PlatformIO pre: script (platformio.ini extra_scripts). 0.24.14.

Compiles LVGL's own source files for speed (-O2) instead of for size (-Os, what the Arduino framework uses for
everything). LVGL is where the drawing time goes: on the YouriTech glass a full repaint took ~90 ms and the screensaver
~120 ms a frame. Only LVGL changes - the rest of the firmware stays -Os, because the app slot is ~92 % full and -O2
makes code bigger. Nothing shared is touched (the framework's ready-made libraries stay as they are).

How: a build "middleware" - PlatformIO calls it for every source file it is about to compile, and for the files under
lvgl/src it hands back the same file with -O2 added to the end of the compiler flags (the last -O on the line wins).
"""
Import("env")


# called for each source file: LVGL's get -O2, everything else is passed back unchanged
def lvgl_for_speed(build_env, node):
    path = node.srcnode().get_path().replace("\\", "/")
    if "/lvgl/src/" not in path:
        return node
    return build_env.Object(node, CCFLAGS=build_env["CCFLAGS"] + ["-O2"])


env.AddBuildMiddleware(lvgl_for_speed)
print("lvgl_speed.py: LVGL compiled with -O2")
