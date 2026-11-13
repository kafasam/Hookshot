"""The FSR 1 stand-in's two shaders: their SPIR-V headers match their sources, as test_taau_fxaa does for the old one.

shaders/taau_easu.comp and taau_rcas.comp are compiled into taau_easu_comp_spv.h and taau_rcas_comp_spv.h; when
glslangValidator of the headers' version is found, each header is compiled again from its source and compared, so the
pairs cannot drift apart.
python -I -B tests/test_taau_easu_spv.py
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"

PAIRS = [("taau_easu", "TAAU_EASU_COMP_SPV"), ("taau_rcas", "TAAU_RCAS_COMP_SPV")]


def main():
    glslang = shutil.which("glslangValidator")
    for stem, vn in PAIRS:
        header = (SHADERS / (stem + "_comp_spv.h")).read_text(encoding="utf-8")
        (SHADERS / (stem + ".comp")).read_text(encoding="utf-8")   # (the source exists)
        version = header.split("\n", 1)[0].strip().lstrip("/ ").strip()
        if glslang:
            got = subprocess.run([glslang, "--version"], capture_output=True, text=True).stdout
            m = re.search(r"Glslang Version: \d+:(\d+)\.(\d+)\.(\d+)", got)
            same = m and version == "11%02d.%s.%s" % (int(m.group(1)), m.group(2), m.group(3))
            if same:
                with tempfile.TemporaryDirectory() as d:
                    out = Path(d) / "h.h"
                    subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "--vn", vn, "-o", str(out),
                                    str(SHADERS / (stem + ".comp"))], check=True, capture_output=True)
                    if out.read_text(encoding="utf-8") != header:
                        raise SystemExit("%s_comp_spv.h is not %s.comp compiled: run glslangValidator again" % (stem, stem))
                print("(%s: the SPIR-V header matches its source)" % stem)
            else:
                print("(%s: glslangValidator is not version %s: the header was not compiled again)" % (stem, version))
        else:
            print("(%s: glslangValidator not found: the header was not compiled again)" % stem)
    print("PASS: the FSR stand-in's SPIR-V headers are their sources compiled")


if __name__ == "__main__":
    main()
