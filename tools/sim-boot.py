#!/usr/bin/env python3
"""Inside the psc-sim container (started by tools/sim-boot.sh): play the boot sequence
on a virtual screen and film it. See sim-boot.sh for what is real and what is a sleep."""
import os, signal, subprocess, threading, time

from PIL import Image, ImageDraw, ImageGrab

RES = os.environ.get('SIM_RES', '1920x1080')
BEFORE = bool(os.environ.get('SIM_BEFORE'))
GAP = float(os.environ.get('SIM_GAP', '2.0'))
OUT = '/out'
PAYLOAD = '/tmp/diag/sim/payload'
ENV = dict(os.environ, DISPLAY=':99', SDL_VIDEODRIVER='x11', SDL_VIDEO_X11_FORCE_EGL='1',
           SDL_AUDIODRIVER='dummy', LIBGL_ALWAYS_SOFTWARE='1')

w, h = RES.split('x')
xvfb = subprocess.Popen(['Xvfb', ':99', '-screen', '0', f'{w}x{h}x24', '-nolisten', 'tcp'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while not os.path.exists('/tmp/.X11-unix/X99'): time.sleep(0.05)

t0 = time.time()
events, frames, stop = [], [], threading.Event()
def ev(what):
    events.append((time.time() - t0, what)); print(f'[{time.time() - t0:6.2f}] {what}', flush=True)

def film():
    while not stop.is_set():
        t = time.time() - t0
        try: im = ImageGrab.grab(xdisplay=':99')
        except Exception: time.sleep(0.05); continue
        frames.append((t, im))
        time.sleep(max(0.0, 0.2 - (time.time() - t0 - t)))
threading.Thread(target=film, daemon=True).start()

def splash_off():
    for p in os.listdir('/proc'):
        try:
            if p.isdigit() and open(f'/proc/{p}/comm').read().strip() == 'sdl_display': os.kill(int(p), signal.SIGTERM)
        except OSError: pass

for f in ('/tmp/lumen-splash-shown', '/tmp/lumen-boot-splash', '/tmp/lumen-presented', '/tmp/lumen-splash.sh'):
    if os.path.exists(f): os.remove(f)

ev("Sony's boot animation ends (black)")
if not BEFORE:
    # start: the splash on Sony's Weston (720p pictures), and the launcher for boot.sh
    open('/tmp/lumen-splash.sh', 'w').write(f'#!/bin/sh\nexec sh "{PAYLOAD}/splash.sh" "$@"\n')
    subprocess.Popen(['sh', '/tmp/lumen-splash.sh'], env=ENV, stderr=open(f'{OUT}/splash.log', 'a'))
    ev('start: boot splash started (splash.sh)')
time.sleep(0.5); ev('start: Sony UI stopped, modules; boot.sh starts')
if BEFORE:
    ev('boot.sh: stops Weston, unpacks the compositor'); time.sleep(1.0)
else:
    time.sleep(1.0); ev('boot.sh: compositor unpacked; splash off, Weston stops'); splash_off()
time.sleep(GAP); ev(f'boot.sh: new compositor up ({GAP:.1f} s, simulated)')
if not BEFORE:
    subprocess.Popen(['sh', '/tmp/lumen-splash.sh', '1080p' if h == '1080' else '720p'], env=ENV,
                     stderr=open(f'{OUT}/splash.log', 'a'))
    ev('boot.sh: boot splash again (1080p pictures)')
time.sleep(1.2); ev('boot.sh: buttons daemon, logs; frontend starts')
if not BEFORE:
    open('/tmp/lumen-boot-splash', 'w').close()
    def watcher():   # boot.sh: the boot splash goes when Lumen's own is on screen
        while not (os.path.exists('/tmp/lumen-splash-shown') or os.path.exists('/tmp/lumen-presented')): time.sleep(0.1)
        time.sleep(0.2); splash_off(); ev("boot.sh: Lumen's splash is up, boot splash closed")
    threading.Thread(target=watcher, daemon=True).start()
t_lumen = time.time()
lumen = subprocess.Popen(['/tmp/app/lumen', '--root', '/media', '--home', '/media/lumen', '--log', f'{OUT}/lumen.log'], env=ENV)
startup = None
while time.time() - t_lumen < 30 and startup is None:
    time.sleep(0.05)
    try:
        for line in open(f'{OUT}/lumen.log'):
            if line.startswith('startup '): startup = line.strip()
    except OSError: pass
ev(f'lumen: dashboard ({startup})')
time.sleep(2.0)
stop.set(); lumen.terminate(); splash_off()
try: lumen.wait(5)
except subprocess.TimeoutExpired: lumen.kill()
xvfb.terminate()

# what was on screen: black stretches, and a contact sheet
def lit(im): return im.convert('L').getextrema()[1] > 12
out, start = [], None
for t, im in frames:
    if not lit(im) and start is None: start = t
    if lit(im) and start is not None: out.append((start, t)); start = None
if start is not None: out.append((start, frames[-1][0]))
t_dash = events[-1][0]
with open(f'{OUT}/timeline.txt', 'w') as f:
    f.write(f'{"before (no boot splash)" if BEFORE else "boot splash"}, {RES}, language {os.environ.get("SIM_LANG")}\n')
    for t, what in events: f.write(f'{t:6.2f} s  {what}\n')
    f.write('black on screen: ' + (', '.join(f'{a:.1f}-{b:.1f} s' for a, b in out) or 'never') +
            f' ({sum(b - a for a, b in out):.1f} s of {t_dash:.1f} s until the dashboard)\n')
print(open(f'{OUT}/timeline.txt').read())
cols, tw = 6, 480
th = tw * int(h) // int(w)
rows = (len(frames) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * (th + 18)), (40, 40, 48))
d = ImageDraw.Draw(sheet)
for i, (t, im) in enumerate(frames):
    x, y = (i % cols) * tw, (i // cols) * (th + 18)
    sheet.paste(im.resize((tw - 4, th - 4), Image.LANCZOS), (x + 2, y + 18))
    d.text((x + 4, y + 3), f'{t:5.2f} s', fill=(255, 255, 255))
    im.save(f'{OUT}/f{int(t * 1000):05d}.png')
sheet.save(f'{OUT}/sheet.png')
