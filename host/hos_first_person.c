/* (hookshot) First person (the Insert menu's Controls row "First person", off by default; HOS_FIRST_PERSON=1 or 0 overrides):
   while Link walks and runs the view is drawn from his eyes. Beside the free camera's hook (the head of the gameplay view's
   own matrix routine, f_804e48c0 after the game stored the view: nx2/oot_patches.py, patch_view), the eye is placed at
   Link's head and the view aimed along the game's own eye-to-target line for the frame — which already carries the yaw, the
   pitch, the mouse and the lock-on easing, so no angle convention of our own is needed: the free camera's atan2 pair
   recovers the angles from (eye - target), the difference its own seed takes, and its writer builds the matrix. Only the
   twelve matrix floats at view+8 are written, exactly those, exactly as the free camera writes them; the eye, the target
   and the up the game stored stay its own.

   The camera itself is untouched — its collision, lock-on and scripted cameras keep running — and every frame a gate below
   does not hold, the game's third-person view is what draws:
   - the view is the gameplay one (camera zero's, as the free camera checks it) and the Debug tab's free camera is off;
   - the player camera updated since this view's last call: f_80644c80's hook (mouse_native.c) hands its camera here each
     time it runs, and a flag it sets and this view clears pairs them. A cutscene, a menu or a loading screen parks the
     camera while its state field would still read 3 or 4, so the state alone is not liveness. An event playing (the
     EventMgr's Main sequence, as the teleport check reads it, host/hos_main.c) is the game's camera too;
   - the camera's state (+0x204, mouse_native.c) is 3 (following Link) or 4 (the player's), the states the mouse camera
     takes. State 2 (an explicit recenter, the Z tap) and state 5 (state 4's sibling) hold the last view at Link's moving
     head for a third of a second rather than pop out and back; state 0 (the aim and shop park: another camera draws,
     traced at Hyrule Castle), 6 (scripted) and the rest are the game's;

   The aim handover (the slingshot, the bow, the hookshot: the game's own first person camera) is the same routine's
   other callers. That camera's view passes this hook too and is not camera zero's, so the gameplay-view check below
   stands it down before anything is written: the game's aim view passes through untouched, every frame of the aim. The
   view this file writes is camera zero's alone, and the moments around the aim are where it must let go:
   - a state not the view's (0 and the rest) drops any held view, so the recenter as the aim lets go (state 2) cannot
     flash where the player looked before he raised it;
   - while another camera's view ran the routine within the hold's third of a second, the hold is dropped too, not
     applied: it would fight the aim's first frames with the view from before it (and a view another camera draws every
     frame keeps it down for good, as an aim does);
   - Link himself updated this frame: his update's hook (f_82c415e0, host/hos_main.c oot_update_player_cheats) hands his
     actor and position here, already validated as his. The view reads the snapshot, never the actor, so a map change that
     frees the old one is a new pointer here, never a dangling read — and a new pointer is held one frame before it is
     trusted. The position must be finite, and the head within a few metres of the game's own target point.

   Known limits, on purpose for a first version: the view keeps the game's camera angles — their downward tilt at rest
   is lifted a little toward level (HOS_FP_PITCH, 0.12 radians) — and their swing when the boom shortens against a wall;
   and the eye is not checked against walls (the game
   still collides its third-person one). The head is the actor's origin (+796, host/hos_main.c) plus so many metres up
   and so many along the view: the menu's First person height (0.5 to 2.5, 1.5: adult Link's eyes; a child's are nearer
   1) and First person offset (0 to 1, 0.15: past the face), live; HOS_FP_UP and HOS_FP_FORWARD win for a run.

   The view once written is told to the renderer (hos_first_person_head below, MkwVkHost's fp_head): the eye it was
   placed at, and that this frame's view is ours — the frame's aim, event and parked-camera gates above all stand it
   down on this side. The renderer's head hide reads exactly that (HOS_FP_HEAD_HIDE_KEYS, gpu/vk/vk_draw.cpp) and hides
   Link's head only while the first person view draws; the game's third person, its aim and its cutscenes keep it. */
#include "hos.h"
#include "../gpu/vk/vk_api.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t oot_gameplay_view(Ctx *c);      /* (hos_main.c: camera zero's view) */
int rx_option(int id, int otherwise);    /* (rx_backend.c) */

enum { STATE_AT = 0x204 };               /* the player camera's state (mouse_native.c: 3 follows Link, 4 the player turns) */
enum { LIMITS_AT = 0x178 };              /* its pitch limits, degrees (mouse_native.c: {-85, 89} in the field: the up one) */
#define VT_VIEW 0x84c18a88ULL            /* the view class's vtable, as the free camera checks it (host/hos_main.c) */
#define EVENTMGR_SLOT 0x84c9e090ULL      /* the EventMgr singleton's slot, as SeqOneShotRequestEvent loads it (host/hos_main.c) */
#define PLAYER_LIVE_NS 250000000LL       /* (Link updated within this: the view uses his position) */
#define HOLD_NS 300000000LL              /* (a recenter or a state-5 moment held this long before the view steps out) */
#define HEAD_FAR 10.0                    /* (m: a head farther than this from the game's target is not his position) */

static volatile LONG64 camera, camera_ran;   /* (the f_80644c80 hook's camera, and that it ran since the view last looked) */
static volatile LONG64 other_view_ns;        /* (another camera's view ran the matrix routine at: the aim's own, a shop's) */

/* f_80644c80's first line (mouse_native.c hos_mouse_camera) hands the camera here too, whatever turns it: the pointer this
   view reads, and the fact it is alive. In its own atomics, beside the hook's own, so the mouse's 150 ms liveness there is
   untouched. From the game's thread, once a frame while that camera runs */
void hos_first_person_camera(uint64_t cam) {
    InterlockedExchange64(&camera, (LONG64)cam);
    InterlockedExchange64(&camera_ran, 1);
}

#if defined(_WIN32) || defined(__linux__)   /* (the debug tools: host/posix has what they call) */
static SRWLOCK fp_lock = SRWLOCK_INIT;   /* (the snapshot and the held view: Link's updates come off varying guest workers) */
static uint64_t player_actor;
static int64_t player_at;
static float player_pos[3];
static double held_yaw, held_pitch;
static int64_t held_at;
/* the renderer's head hide (gpu/vk/vk_draw.cpp, asked through MkwVkHost's fp_head): the eye of the last view this file
   wrote, and that it wrote one, in the serial of the write. Cleared at every call's start below and set again only once
   every gate has passed, so the hide is on exactly the frames the view is ours — the aim, an event, a parked state and
   the row off all return before the set, and the game runs a view through this hook every frame it renders */
static volatile LONG64 fp_head_serial;
static volatile LONG64 fp_head_eye[3];   /* (the eye, as its double bits: there is no atomic of a double) */

/* Link's own update (f_82c415e0's hook, host/hos_main.c oot_update_player_cheats) hands his actor and position here each
   frame, on the game's thread, already validated as his (hos_oot_lift_player's checks) */
void hos_first_person_player(uint64_t actor, const float position[3]) {
    AcquireSRWLockExclusive(&fp_lock);
    player_actor = actor;
    player_at = hos_now_ns();
    memcpy(player_pos, position, sizeof player_pos);
    ReleaseSRWLockExclusive(&fp_lock);
}

/* the menu's row (option 47) or HOS_FIRST_PERSON, as the mouse camera's own switch */
static int fp_on(void) {
    static const char *env;
    static int looked;
    if (!looked) looked = 1, env = getenv("HOS_FIRST_PERSON");
    return env ? atoi(env) != 0 : rx_option(HOS_OPT_FIRST_PERSON, 0);
}

/* the head's height over Link's origin and its offset along the view, in metres: the menu's rows (First person height
   and First person offset, in hundredths of a metre), read at every view so a slider shows at once, as the Camera speed
   row's own does at each present (host/nn_vi.c hos_camera_scale); HOS_FP_UP or HOS_FP_FORWARD, when set to a value,
   wins for the run. A row out of its range, as without the renderer's menu, is its default */
static double fp_up(void) {
    static const char *env;
    static double forced = -1.0;
    static int looked;
    if (!looked) {
        looked = 1, env = getenv("HOS_FP_UP");
        if (env && *env) {
            forced = atof(env);
            if (!(forced > 0.0 && forced < 100.0)) forced = -1.0;   /* (and NaN: not a value) */
        }
    }
    if (forced >= 0.0) return forced;
    const int row = rx_option(HOS_OPT_FP_HEIGHT, 150);
    return (row >= 50 && row <= 250 ? row : 150) / 100.0;
}

static double fp_forward(void) {
    static const char *env;
    static double forced = -1.0;
    static int looked;
    if (!looked) {
        looked = 1, env = getenv("HOS_FP_FORWARD");
        if (env && *env) {
            forced = atof(env);
            if (!(forced >= 0.0 && forced < 10.0)) forced = -1.0;
        }
    }
    if (forced >= 0.0) return forced;
    const int row = rx_option(HOS_OPT_FP_OFFSET, 15);
    return (row >= 0 && row <= 100 ? row : 15) / 100.0;
}

/* the pitch lifted from the game's line toward level, in radians (HOS_FP_PITCH, 0.12: about seven degrees): the follow
   camera looks down at Link, so its eye-to-target line carries the rest tilt, and at his eyes that is a bias, not a
   view. Conservative on purpose — the rows above place the eye and are the player's dial to turn; this only takes the
   systematic downward angle out, and (below) never past the camera's own up limit */
static double fp_pitch_correct(void) {
    static double v = -1;
    if (v < 0) {
        const char *e = getenv("HOS_FP_PITCH");
        v = e && *e ? atof(e) : 0.12;
        if (!(v > -1.5 && v < 1.5)) v = 0.12;   /* (and NaN: not a value) */
    }
    return v;
}

/* an event (a cutscene) playing: the EventMgr singleton's Main sequence, the expression the teleport check logs
   (host/hos_main.c oot_teleport_followup) */
static int event_playing(Ctx *c) {
    const uint64_t holder = recomp_load64(c, EVENTMGR_SLOT);
    const uint64_t events = holder ? recomp_load64(c, holder) : 0;
    return events && recomp_load64(c, events + 104) != 0;
}

/* Beside the free camera's hook (nx2/oot_patches.py patch_view inserts them side by side): view is the view the game just
   stored, on the game's thread, once a call of the matrix routine */
void hos_first_person(Ctx *c, uint64_t view) {
    static uint64_t held_actor;
    static long long applied;
    /* the head hide's answer for this frame starts off: set again only where the view is written below, so whichever
       gate returns first leaves the head to the game's camera, and the hide follows the view within a frame of the
       row's toggle (the renderer replays a frame behind the write anyway, and takes that skew with it) */
    InterlockedExchange64(&fp_head_serial, 0);
    if (!view || recomp_load64(c, view) != VT_VIEW) return;
    /* camera zero supplies the gameplay view; the same matrix routine also serves other cameras (the free camera's
       check). One of those is the aim (the slingshot's, the bow's own first person camera): its view is not this one,
       and nothing of ours touches it — only remembered, so the hold below stands down while an aim is around */
    if (oot_gameplay_view(c) != view) {
        InterlockedExchange64(&other_view_ns, hos_now_ns());
        return;
    }
    if (rx_option(HOS_OPT_CHEAT_CAMERA, 0)) return;   /* (the free camera owns the view: its read of the option and this
                                                          one can straddle a mid-frame toggle by a frame) */
    if (!fp_on()) return;
    /* alive: the player camera's update ran since this view's last call (the flag it sets here, cleared by this read).
       Taken only once the view is the gameplay one, so another camera's pass through the same routine does not eat it */
    if (!InterlockedExchange64(&camera_ran, 0)) return;
    if (event_playing(c)) return;
    const uint64_t cam = (uint64_t)InterlockedCompareExchange64(&camera, 0, 0);
    if (!cam) return;
    uint32_t state;
    memcpy(&state, GP(cam + STATE_AT), 4);
    if (state != 2 && state != 3 && state != 4 && state != 5) {
        /* (each state that leaves the view to the game's camera, once — what Z-targeting, climbing and swimming turn out
           to be, to add here if one of them should stay, as mouse_native.c logs its own) */
        static uint32_t said[8];
        static int nsaid;
        int seen = 0;
        for (int i = 0; i < nsaid; i++) seen |= said[i] == state;
        if (!seen && nsaid < 8) {
            said[nsaid++] = state;
            hos_log("first person: camera state %u: the game's camera draws", state);
        }
        /* the aim and shop park, a scripted camera: any held view ends here, so the recenter after it (state 2) cannot
           flash where the player looked before the aim began — the game's camera draws until he walks again */
        AcquireSRWLockExclusive(&fp_lock);
        held_at = 0;
        ReleaseSRWLockExclusive(&fp_lock);
        return;
    }
    /* the game's geometry for this frame (the free camera seeds its own angles from the same two points) */
    double eye[3], target[3];
    memcpy(eye, A64_PTR(view + 96), sizeof eye);
    memcpy(target, A64_PTR(view + 120), sizeof target);
    AcquireSRWLockExclusive(&fp_lock);
    const int64_t now = hos_now_ns();
    double yaw, pitch;
    if (state == 2 || state == 5) {
        /* an explicit recenter (the Z tap) or state 4's sibling: hold the last view at the moving head rather than pop out
           and back, and give the game its camera back if the state stays. Not across another camera: while one drew
           within the hold's span (the aim's own view, passing the same routine above), the held view is from before it
           and would fight the aim's first frames — it is dropped, and a view another camera draws every frame keeps it
           down for as long as the aim lasts */
        if (now - (int64_t)InterlockedCompareExchange64(&other_view_ns, 0, 0) < HOLD_NS || now - held_at >= HOLD_NS) {
            held_at = 0;
            ReleaseSRWLockExclusive(&fp_lock);
            return;
        }
        yaw = held_yaw, pitch = held_pitch;
    } else {
        /* 3 (following Link) or 4 (the player's): the game's line, from its own eye to its own target. The difference is
           the back vector — the matrix's third row below — so the angles come out facing where the game looks */
        const double x = eye[0] - target[0], y = eye[1] - target[1], z = eye[2] - target[2];
        if (x * x + y * y + z * z < 1e-12) {   /* (the game's own degenerate case: eye and target together) */
            ReleaseSRWLockExclusive(&fp_lock);
            return;
        }
        yaw = atan2(x, z), pitch = atan2(y, hypot(x, z));
        /* the rest tilt out: the correction lifts the direction toward level before the matrix is built from it, never
           past the camera's own up limit (+0x178, degrees) — the game would not show a view above that either */
        {
            float lo;
            memcpy(&lo, GP(cam + LIMITS_AT), 4);
            const double least = (double)lo * (3.14159265358979323846 / 180.0);
            pitch -= fp_pitch_correct();
            if (pitch < least) pitch = least;
        }
        held_yaw = yaw, held_pitch = pitch, held_at = now;
    }
    /* Link's head: the snapshot his own update left. A new actor (a door, a loading transition) is held a frame before the
       view trusts it, and no fresh player at all is a screen without him */
    if (!player_actor || now - player_at > PLAYER_LIVE_NS || player_actor != held_actor) {
        held_actor = player_actor;
        ReleaseSRWLockExclusive(&fp_lock);
        return;
    }
    double head[3] = {player_pos[0], player_pos[1] + fp_up(), player_pos[2]};
    ReleaseSRWLockExclusive(&fp_lock);
    if (!isfinite(head[0]) || !isfinite(head[1]) || !isfinite(head[2])) return;
    const double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
    const double back[3] = {cp * sy, sp, cp * cy};   /* (where the view looks, reversed: the forward offset goes against it) */
    const double forward = fp_forward();
    for (int i = 0; i < 3; i++) head[i] -= back[i] * forward;
    {   /* (the game's target is near Link in play: a head far from it is not his position this frame) */
        const double dx = head[0] - target[0], dy = head[1] - target[1], dz = head[2] - target[2];
        if (dx * dx + dy * dy + dz * dz > HEAD_FAR * HEAD_FAR) return;
    }
    /* the view matrix, the free camera's own writer's rows and columns (oot_build_free_view): the axes from the angles,
       the last column -axis . head */
    const double axes[3][3] = {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {back[0], back[1], back[2]}};
    for (int row = 0; row < 3; row++) {
        const double *axis = axes[row];
        const float values[4] = {(float)axis[0], (float)axis[1], (float)axis[2],
            (float)-(axis[0] * head[0] + axis[1] * head[1] + axis[2] * head[2])};
        for (int col = 0; col < 4; col++) {
            uint32_t bits;
            memcpy(&bits, &values[col], sizeof bits);
            recomp_store32(c, view + 8 + (row * 4 + col) * 4, bits);
        }
    }
    /* the head hide's answer: on, at this eye, with this write's serial (the eye bits first and the serial last, so a
       reader that sees a serial sees an eye of the same write — a frame straddled mid-read is one frame's skew, taken) */
    applied++;
    {
        uint64_t bits[3];
        memcpy(bits, head, sizeof bits);
        for (int i = 0; i < 3; i++) InterlockedExchange64(&fp_head_eye[i], (LONG64)bits[i]);
        InterlockedExchange64(&fp_head_serial, (LONG64)applied);
    }
    if ((applied & 4095) == 1)
        hos_log("first person: %lld views at Link's head; yaw %.2f pitch %.2f", applied, yaw, pitch);
}

/* (MkwVkHost's fp_head, host/rx_backend.c's table) the head hide's answer above, for the renderer: the serial of the
   last view written (>0) and the eye it was placed at, in eye, while the first person view owns the frame's view; 0
   when the game's own camera draws it — the aim, an event, a parked state, the row off — and eye left alone */
int hos_first_person_head(double eye[3]) {
    const LONG64 serial = InterlockedCompareExchange64(&fp_head_serial, 0, 0);
    if (serial <= 0) return 0;
    uint64_t bits[3];
    for (int i = 0; i < 3; i++) bits[i] = (uint64_t)InterlockedCompareExchange64(&fp_head_eye[i], 0, 0);
    memcpy(eye, bits, sizeof bits);
    return (int)serial;
}
#else
void hos_first_person(Ctx *c, uint64_t view) {
    (void)c;
    (void)view;
}
int hos_first_person_head(double eye[3]) {
    (void)eye;
    return 0;
}
#endif
