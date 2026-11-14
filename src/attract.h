/* Screensaver / attract scene and demo picker (docs/settings-design.md 10).
 *
 * OFF --idle >= screensaver_minutes--> ENTER (1.2 s) --> IDLE
 * IDLE --demos on, candidates, attract_demo_delay--> DEMO_OUT (1.5 s) --> launch_demo
 * any state --input--> LEAVE (0.6 s) --> OFF   (that input never reaches the carousel)
 */
#ifndef LUMEN_ATTRACT_H
#define LUMEN_ATTRACT_H

typedef struct {
   int   move, plat_move;   /* carousel steps requested this frame (auto-rotation) */
   int   launch_demo;       /* 1 on the frame DEMO_OUT ends: exit 11 with attract_demo_game() */
   float amount;            /* 0..1 how much of the attract scene is on screen */
   float dim;               /* alpha of the black quad drawn after the 3D pass */
} attract_out_t;

void  attract_init(int start_active, const char *home);   /* after the library scan */
void  attract_note_input(double t);                 /* any input this frame */
int   attract_active(void);                         /* ENTER/IDLE/DEMO_OUT */
void  attract_start(double t);
void  attract_update(double t, float dt, int can_start, attract_out_t *out);
void  attract_case_offset(int o, double t, float *dx, float *dy, float *dry);
void  attract_bg_target(const float base[3], double t, float out[3]);
void  attract_draw(double t);                       /* PRESS START / demo title, in the 2D block */
int   attract_pick_demo(void);                      /* g_games index or -1 */
int   attract_demo_game(void);                      /* game chosen for the demo being launched, or -1 */

#endif
