# Post-build: turn firmware.hex into firmware.uf2 for drag-and-drop flashing.
import sys
from os.path import join
Import("env")

def hex_to_uf2(source, target, env):
    fw = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
    conv = join(fw, "tools", "uf2conv", "uf2conv.py")
    hex_path = target[0].get_abspath()
    uf2_path = hex_path.replace(".hex", ".uf2")
    env.Execute(f'"{sys.executable}" "{conv}" "{hex_path}" -c -f 0xADA52840 -o "{uf2_path}"')

env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", hex_to_uf2)
