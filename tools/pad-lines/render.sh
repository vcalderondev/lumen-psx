#!/bin/sh
# render.sh <model> <query>: frames into out/<model>/NNN.png; the local server and Chrome are stopped at the end
S=$(cd "$(dirname "$0")" && pwd); M=$1; shift
pkill -f "render3d/server.py" 2>/dev/null
rm -rf "$S/out/$M"
python3 -I "$S/server.py" "$MODELS" "$S/out" > "$S/server.log" 2>&1 & SP=$!
sleep 1
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --remote-debugging-port=9333 --user-data-dir="$S/chrome-profile" --window-size=3000,2200 "http://127.0.0.1:8791/index.html?name=$M&$1" >/dev/null 2>&1 & CP=$!
i=0; while kill -0 $SP 2>/dev/null && [ $i -lt ${TIMEOUT:-150} ]; do sleep 1; i=$((i + 1)); done
kill $CP 2>/dev/null; kill $SP 2>/dev/null
echo "$M: $(ls "$S/out/$M" 2>/dev/null | wc -l | tr -d ' ') frames in ${i}s"
