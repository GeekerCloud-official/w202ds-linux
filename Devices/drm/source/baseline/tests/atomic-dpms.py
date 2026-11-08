#!/usr/bin/env python3
"""Run the real commit tails/helpers with a deterministic DPMS interleaving.

The harness supplies a single CRTC and simulated IRQ completions. It extracts
the functions from the selected kernel and this driver rather than duplicating
their wait logic. This tests commit lifetime/ordering, not display hardware.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"(?:static\s+)?void\s+" + name + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"function not found: {name}")
    begin = source.index("{", match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HZ 100
struct completion { bool done; };
struct drm_crtc {
    struct { int id; } base;
    const char *name;
    unsigned count;
};
struct drm_crtc_state { bool active; };
struct drm_crtc_commit { struct completion flip_done; };
struct drm_device {
    struct { int num_crtc; } mode_config;
    struct { int queue; } vblank[1];
};
struct drm_atomic_state {
    struct drm_device *dev;
    bool legacy_cursor_update;
    struct {
        struct drm_crtc *ptr;
        struct drm_crtc_commit *commit;
        unsigned last_vblank_count;
    } crtcs[1];
    struct drm_crtc_state old_state, new_state;
    struct drm_crtc_commit *fake_commit;
};
static struct drm_crtc crtc;
static struct drm_crtc_commit commit;
static bool scanout, stop_after_flip, delayed_event, hw_done, injected;
static unsigned vblank_timeouts, flip_timeouts, delayed_completions, cleaned;

#define for_each_oldnew_crtc_in_state(s, c, o, n, i) \
    for ((i) = 0; (i) < 1 && ((c) = (s)->crtcs[i].ptr, \
         (o) = &(s)->old_state, (void)(o), (n) = &(s)->new_state, 1); (i)++)
#define for_each_old_crtc_in_state(s, c, o, i) \
    for ((i) = 0; (i) < 1 && ((c) = (s)->crtcs[i].ptr, \
         (o) = &(s)->old_state, (void)(o), 1); (i)++)
#define drm_crtc_mask(c) ((void)(c), 1u)
#define drm_crtc_vblank_get(c) ((void)(c), 0)
#define drm_crtc_vblank_put(c) ((void)(c))
#define drm_crtc_vblank_count(c) ((c)->count)
#define msecs_to_jiffies(ms) (ms)
#define WARN(condition, ...) ((condition) ? (++vblank_timeouts, 1) : 0)
#define drm_err(dev, ...) ((void)(dev), ++flip_timeouts)

static void next_disable(void)
{
    if (stop_after_flip && hw_done && commit.flip_done.done) {
        scanout = false;
        injected = true;
    }
}
static void wait_vblank(void)
{
    next_disable();
    if (scanout)
        crtc.count++;
}
#define wait_event_timeout(queue, condition, timeout) \
    ((void)(queue), (void)(timeout), wait_vblank(), !!(condition))
static int wait_for_completion_timeout(struct completion *c, unsigned timeout)
{
    (void)timeout;
    next_disable();
    if (!c->done && scanout && delayed_event) {
        /* The optional arm-vblank path completes on a later frame. */
        crtc.count++;
        c->done = true;
        delayed_completions++;
    }
    return c->done;
}
static void complete_all(struct completion *c) { c->done = true; }
static void drm_atomic_helper_commit_modeset_disables(struct drm_device *d,
                                                     struct drm_atomic_state *s)
{
    (void)d;
    if (s->old_state.active && !s->new_state.active) {
        scanout = false;
        /* w202_disable sends the synthetic event after stopping scanout. */
        complete_all(&commit.flip_done);
    }
}
static void drm_atomic_helper_commit_planes(struct drm_device *d,
                                            struct drm_atomic_state *s, int flags)
{
    (void)d;
    assert(flags == 0);
    if (s->new_state.active) {
        scanout = true;
        if (!delayed_event)
            complete_all(&commit.flip_done); /* layer-update IRQ */
    }
}
static void drm_atomic_helper_commit_modeset_enables(struct drm_device *d,
                                                    struct drm_atomic_state *s)
{
    (void)d;
    assert(!s->new_state.active || scanout);
}
static void drm_atomic_helper_fake_vblank(struct drm_atomic_state *s) { (void)s; }
static void drm_atomic_helper_commit_hw_done(struct drm_atomic_state *s)
{
    (void)s;
    hw_done = true;
}
static void drm_atomic_helper_cleanup_planes(struct drm_device *d,
                                             struct drm_atomic_state *s)
{
    (void)d;
    (void)s;
    assert(hw_done);
    assert(commit.flip_done.done); /* never release a buffer before its event */
    cleaned++;
}

/* REAL_FUNCTIONS */

static void run(void (*tail)(struct drm_atomic_state *), bool old_active,
                bool active, bool delay, bool dpms)
{
    struct drm_device dev = { .mode_config.num_crtc = 1 };
    struct drm_atomic_state state = {
        .dev = &dev, .old_state.active = old_active, .new_state.active = active,
        .crtcs = {{ .ptr = &crtc, .commit = &commit }},
    };
    memset(&commit, 0, sizeof(commit));
    crtc = (struct drm_crtc){ .base.id = 36, .name = "crtc-0" };
    scanout = old_active;
    stop_after_flip = dpms;
    delayed_event = delay;
    hw_done = injected = false;
    vblank_timeouts = flip_timeouts = delayed_completions = cleaned = 0;
    if (!old_active && !active)
        complete_all(&commit.flip_done); /* setup_commit: stays inactive */
    tail(&state);
    assert(cleaned == 1);
    assert(flip_timeouts == 0);
}
int main(void)
{
    run(drm_atomic_helper_commit_tail, true, true, false, true);
    assert(injected && vblank_timeouts == 1);
    puts("PASS: old tail reproduces timeout when DPMS stops the next vblank");
    run(w202_atomic_commit_tail, true, true, false, true);
    assert(injected && vblank_timeouts == 0);
    puts("PASS: completed latch remains safe when the next commit stops scanout");
    run(w202_atomic_commit_tail, true, true, true, false);
    assert(delayed_completions == 1 && vblank_timeouts == 0);
    puts("PASS: deferred vblank event completes before framebuffer cleanup");
    run(w202_atomic_commit_tail, true, false, false, false);
    assert(!scanout && vblank_timeouts == 0);
    puts("PASS: disable finishes through its synthetic completion");
    run(w202_atomic_commit_tail, false, false, false, false);
    assert(!scanout && vblank_timeouts == 0);
    puts("PASS: inactive commit needs no hardware vblank");
    run(w202_atomic_commit_tail, false, true, false, false);
    assert(scanout && vblank_timeouts == 0);
    puts("PASS: wake retires its buffer after the layer completion");
    run(drm_atomic_helper_commit_tail, true, true, false, false);
    assert(vblank_timeouts == 0);
    puts("PASS: baseline has no timeout while scanout keeps running");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    kernel_source = (args.kernel / "drivers/gpu/drm/drm_atomic_helper.c").read_text()
    driver_source = (Path(__file__).resolve().parents[1] / "w202_r6p0_main.c").read_text()
    names = ("drm_atomic_helper_wait_for_vblanks", "drm_atomic_helper_wait_for_flip_done",
             "drm_atomic_helper_commit_tail")
    actual = "\n\n".join(function(kernel_source, n) for n in names)
    actual += "\n\n" + function(driver_source, "w202_atomic_commit_tail")
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.out / "atomic-dpms.c"
    binary = args.out / "atomic-dpms"
    source.write_text(HARNESS.replace("/* REAL_FUNCTIONS */", actual))
    subprocess.run([*shlex.split(args.cc), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
