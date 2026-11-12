"""First person's handover to the game's own first person aim (host/hos_first_person.c).

The file is compiled on its own against a fake guest memory that holds the gameplay view (camera zero's), a second view
(the aim camera's), the player camera and Link. hos_first_person is what the view's matrix routine calls for every view
it serves (nx2/oot_patches.py patch_view). The aim's own view passes untouched — another camera's view is only
remembered — and the hold of the last good view (a recenter, state 2) neither fights the aim's frames nor flashes the
view from before it once the aim lets go: a state not the view's drops the hold, and another camera drawing within the
hold's span drops it too. The states that drive the view (3 and 4) still write it, at Link's head as the menu's rows
place it, live, with the follow camera's rest tilt lifted toward level (HOS_FP_PITCH, 0.12) and never past the camera's
own up limit.
python -I -B tests/test_first_person_view.py   (Linux: host/posix's windows.h)
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]

HOS_H = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t *fake_heap;
#define HOS_HEAP_BASE 0x1000000000ULL
#define GP(a) ((void *)(fake_heap + ((uint64_t)(a) - HOS_HEAP_BASE)))
extern uint64_t hos_heap_size;
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define A64_PTR(a) GP(a)
extern uint64_t recomp_load64(Ctx *c, uint64_t a);
extern void recomp_store32(Ctx *c, uint64_t a, uint32_t v);
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
"""

FIXTURE = r"""
#include "hos.h"
#include "../gpu/vk/vk_api.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
uint8_t *fake_heap;
uint64_t hos_heap_size = 64 * 4096;
static int64_t clock_ns = 1000000000LL;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc(10, stderr); }
int64_t hos_now_ns(void) { return clock_ns; }

/* the guest as the hook sees it: camera zero's view, the aim camera's own, the player camera, Link */
#define VT_VIEW 0x84c18a88ULL
static const uint64_t VIEW = HOS_HEAP_BASE + 12 * 4096, AIM_VIEW = HOS_HEAP_BASE + 14 * 4096;
static const uint64_t CAM = HOS_HEAP_BASE + 10 * 4096 + 0x40, ACTOR = HOS_HEAP_BASE + 4 * 4096;
uint64_t recomp_load64(Ctx *c, uint64_t a) {
    (void)c;
    if (a == VIEW || a == AIM_VIEW) return VT_VIEW;   /* (the view class's vtable, as the hook checks it) */
    return 0;   /* (the EventMgr's slot empty: no cutscene playing) */
}
void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; memcpy(GP(a), &v, 4); }
uint64_t oot_gameplay_view(Ctx *c) { (void)c; return VIEW; }   /* (camera zero's: the aim camera's is another) */

/* the menu's rows, as the renderer answers them */
static int menu_fp = 1, menu_height = 150, menu_offset = 15, menu_free_camera;
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_FIRST_PERSON) return menu_fp;
    if (id == HOS_OPT_FP_HEIGHT) return menu_height;
    if (id == HOS_OPT_FP_OFFSET) return menu_offset;
    if (id == HOS_OPT_CHEAT_CAMERA) return menu_free_camera;
    return otherwise;
}
void hos_first_person_camera(uint64_t cam);
void hos_first_person_player(uint64_t actor, const float position[3]);
void hos_first_person(Ctx *c, uint64_t view);
int hos_first_person_head(double eye[3]);   /* (what the renderer's head hide reads: MkwVkHost fp_head) */

static int camera_state = 4;   /* the player camera's state, at +0x204 */
static float link_pos[3] = {1, 2, 3};
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }
static void state(int s) { memcpy(GP(CAM + 0x204), &s, 4); camera_state = s; }
static void line(float ey, float ty) {   /* the game's eye and target the views stored: looking due -z, tilted by the line */
    const double eye[3] = {0, ey, 10}, target[3] = {0, ty, 0};   /* (as the free camera's own seed reads them) */
    memcpy(GP(VIEW + 96), eye, sizeof eye), memcpy(GP(VIEW + 120), target, sizeof target);
    memcpy(GP(AIM_VIEW + 96), eye, sizeof eye), memcpy(GP(AIM_VIEW + 120), target, sizeof target);
}
/* one frame: the camera's update, Link's update, then the routine over the views (the aim camera's first, when it draws) */
static void frame(uint64_t view, int aim_drew) {
    clock_ns += 16666667;
    state(camera_state);
    hos_first_person_camera(CAM);
    hos_first_person_player(ACTOR, link_pos);
    Ctx c;
    memset(&c, 0, sizeof c);
    if (aim_drew) hos_first_person(&c, AIM_VIEW);
    hos_first_person(&c, view);
}
static float mf(uint64_t at, int i) { float f; memcpy(&f, GP(at + 8 + i * 4), 4); return f; }
static void game_view(uint64_t at) { for (int i = 0; i < 12; i++) putf(at + 8 + i * 4, 12345); }
static int mine(uint64_t at) { return mf(at, 0) != 12345; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int close_to(float a, float b) { return fabsf(a - b) < 1e-3f; }
#define PITCH_CORRECT 0.12   /* the rest tilt taken out (HOS_FP_PITCH's default, host/hos_first_person.c) */
#define LINE_LEVEL (10 * tan(PITCH_CORRECT))   /* an eye this high over the target: the corrected pitch is level */

int main(void) {
    fake_heap = (uint8_t *)calloc(64, 4096);
    uint64_t vt = VT_VIEW;
    memcpy(GP(VIEW), &vt, 8), memcpy(GP(AIM_VIEW), &vt, 8);
    putf(CAM + 0x178, -85), putf(CAM + 0x17c, 89);   /* (the camera's pitch limits, as the game keeps them) */

    /* walking (state 4): the view at Link's head, placed by the menu's rows */
    line(LINE_LEVEL, 0);
    frame(VIEW, 0);   /* (a new actor is held a frame before the view trusts it) */
    game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW));   /* the twelve floats are ours */
    /* the line corrected is yaw 0 pitch 0: back (0,0,1), the head (1, 2+up, 3-offset) */
    CHECK(close_to(mf(VIEW, 0), 1) && close_to(mf(VIEW, 1), 0) && close_to(mf(VIEW, 2), 0) && close_to(mf(VIEW, 3), -1));
    CHECK(close_to(mf(VIEW, 7), -(2 + 1.5)) && close_to(mf(VIEW, 11), -(3 - 0.15)));
    /* the head hide's answer: a serial (>0) while the view is ours, and the eye it was placed at — the head the rows
       built (1, 2+1.5, 3), the offset taking it back along the level view's back vector (0,0,1) to z 3-0.15 */
    {
        double eye[3] = {123, 123, 123};
        const int serial = hos_first_person_head(eye);
        CHECK(serial > 0);
        CHECK(close_to(eye[0], 1) && close_to(eye[1], 2 + 1.5) && close_to(eye[2], 3 - 0.15));
        game_view(VIEW), frame(VIEW, 0);
        CHECK(hos_first_person_head(eye) == serial + 1);   /* (each written view: the next serial) */
    }
    menu_height = 200;   /* the rows are live: the next frame takes the new height */
    game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW) && close_to(mf(VIEW, 7), -(2 + 2.0)));
    menu_height = 150;

    /* the rest tilt taken out: a line pitched down by atan2(3,10) shows the correction's 0.12 less of it, and a line
       already above where the correction would point stops at the camera's own up limit */
    state(4), line(3, 0);
    game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW) && close_to(mf(VIEW, 9), sin(atan2(3, 10) - PITCH_CORRECT)));   /* row 2's back vector, its y */
    putf(CAM + 0x178, -5);   /* (an up limit five degrees over level, as the camera keeps it) */
    line(-1, 0);   /* (the game's line looks up: atan2(-1,10), and the correction would point past the limit) */
    game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW) && close_to(mf(VIEW, 9), sin(-5 * 3.14159265358979 / 180)));
    putf(CAM + 0x178, -85), line(LINE_LEVEL, 0);

    /* the recenter (the Z tap, state 2) within the hold: the last view held, not this frame's line */
    game_view(VIEW), frame(VIEW, 0);   /* (state 4 still: the hold seeded from the level line) */
    state(2), line(5, 0);   /* (the game's line changed under the recenter) */
    game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW) && close_to(mf(VIEW, 7), -(2 + 1.5)));   /* held: the view from before, level, not the new line */

    /* the aim: the game's own first person camera draws (another view), ours stands down at once */
    state(0);
    game_view(VIEW), game_view(AIM_VIEW), frame(VIEW, 1);
    CHECK(!mine(VIEW) && !mine(AIM_VIEW));   /* the aim's view and camera zero's both untouched */
    {   /* and the head hide's answer with it: 0, the head stays the game's to draw */
        double eye[3];
        CHECK(hos_first_person_head(eye) == 0);
    }

    /* the aim lets go straight to a recenter (state 2, no walking between): no flash of the view from before it. The
       aim's last pass is older than the hold now, so what stands down is the park's end of the held view */
    clock_ns += 400000000LL;
    state(2), line(LINE_LEVEL, 0);
    game_view(VIEW), frame(VIEW, 0);
    CHECK(!mine(VIEW));
    /* and while an aim is close — its view a frame ago — a recenter does not pick the hold up either: it would fight
       the aim's frames with the view from before it */
    state(4), game_view(VIEW), frame(VIEW, 0);   /* (walking again: the view ours, the hold seeded fresh) */
    state(2);
    game_view(VIEW), game_view(AIM_VIEW), frame(VIEW, 1);
    CHECK(!mine(VIEW));

    /* the aim's view alone (camera zero parked deeper): passed through, nothing written to it */
    game_view(AIM_VIEW);
    { Ctx c; memset(&c, 0, sizeof c); hos_first_person(&c, AIM_VIEW); }
    CHECK(!mine(AIM_VIEW));

    /* walking again after all that: the view ours at once */
    state(3), line(LINE_LEVEL, 0), game_view(VIEW), frame(VIEW, 0);
    CHECK(mine(VIEW) && close_to(mf(VIEW, 7), -(2 + 1.5)));

    /* the switches: the free camera owns the view, the row off: nothing written */
    state(4), menu_free_camera = 1;
    game_view(VIEW), frame(VIEW, 0);
    CHECK(!mine(VIEW));
    {   double eye[3]; CHECK(hos_first_person_head(eye) == 0); }   /* (the hide stood down with the view) */
    menu_free_camera = 0, menu_fp = 0;
    game_view(VIEW), frame(VIEW, 0);
    CHECK(!mine(VIEW));
    {   double eye[3]; CHECK(hos_first_person_head(eye) == 0); }   /* (the row off: unhidden within the frame) */
    menu_fp = 1;
    game_view(VIEW), frame(VIEW, 0);   /* (back on: the serial is again a positive one, the eye with it) */
    {
        double eye[3] = {123, 123, 123};
        CHECK(hos_first_person_head(eye) > 0 && close_to(eye[1], 2 + 1.5));
    }

    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class FirstPersonViewTests(unittest.TestCase):
    def test_the_aim_view_passes_through(self):
        if sys.platform == "win32":
            self.skipTest("host/posix's windows.h stands in for Windows here")
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-first-person-") as tmp:
            tmp = Path(tmp)
            (tmp / "host").mkdir()
            (tmp / "gpu" / "vk").mkdir(parents=True)
            (tmp / "gpu" / "render").mkdir(parents=True)
            # (beside the stand-in hos.h, so its #include "hos.h" finds that one; vk_api.h where its ../gpu/vk finds it)
            (tmp / "host" / "hos.h").write_text(HOS_H, encoding="utf-8")
            shutil.copy(ROOT / "host" / "hos_first_person.c", tmp / "host" / "hos_first_person.c")
            shutil.copy(ROOT / "gpu" / "vk" / "vk_api.h", tmp / "gpu" / "vk" / "vk_api.h")
            for header in ("rx_nvn.h", "nvn_api.h"):
                shutil.copy(ROOT / "gpu" / "render" / header, tmp / "gpu" / "render" / header)
            (tmp / "host" / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t"
            build = subprocess.run([compiler, "-std=gnu11", "-include", ENV_ON_H, "-Wall", "-O1", "-g",
                                    "-I", str(tmp / "host"), "-I", str(ROOT / "host" / "posix"),
                                    str(tmp / "host" / "t.c"), str(tmp / "host" / "hos_first_person.c"),
                                    "-o", str(binary), "-lm"],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
