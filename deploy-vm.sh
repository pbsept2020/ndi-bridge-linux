#!/bin/bash
VM="Ubuntu 22.04.2 (x86_64 emulation) (1)"
SRC=~/Projets/ndi-bridge-linux

echo "=== 1/5 Killing ALL old processes ==="
prlctl exec "$VM" sudo killall -9 ndi-bridge ndi-viewer 2>/dev/null
prlctl exec "$VM" sudo pkill -9 -f ndi-bridge 2>/dev/null
sleep 2

echo "=== 2/5 Copying ALL source files ==="
for f in $(cd "$SRC" && find src -name "*.cpp" -o -name "*.h" | sort) CMakeLists.txt cmake/FindNDI.cmake; do
  if [ -f "$SRC/$f" ]; then
    prlctl exec "$VM" tee /home/parallels/ndi-bridge-linux/$f < "$SRC/$f" > /dev/null 2>&1
  fi
done

echo "=== 3/5 Clean build ==="
prlctl exec "$VM" rm -rf /home/parallels/ndi-bridge-linux/build/CMakeFiles/ndi_bridge_common.dir 2>&1
prlctl exec "$VM" rm -rf /home/parallels/ndi-bridge-linux/build/CMakeFiles/ndi-bridge.dir 2>&1
prlctl exec "$VM" rm -rf /home/parallels/ndi-bridge-linux/build/CMakeFiles/ndi-test-pattern.dir 2>&1
prlctl exec "$VM" cmake -B /home/parallels/ndi-bridge-linux/build /home/parallels/ndi-bridge-linux 2>&1 | tail -5
prlctl exec "$VM" cmake --build /home/parallels/ndi-bridge-linux/build 2>&1 | tail -15

echo "=== 4/5 Setting permissions ==="
prlctl exec "$VM" sudo chmod +x /home/parallels/ndi-bridge-linux/build/ndi-bridge /home/parallels/ndi-bridge-linux/build/ndi-viewer /home/parallels/ndi-bridge-linux/build/ndi-test-pattern 2>&1

echo "=== 5/6 Launching join ==="
# Write launcher script to VM (prlctl exec + nohup + bash -c doesn't work)
printf '#!/bin/bash\nsudo killall -9 ndi-bridge 2>/dev/null\nsleep 1\n/home/parallels/ndi-bridge-linux/build/ndi-bridge join --name "P20064 Bridge" --port 5990 -v > /tmp/join.log 2>&1 &\nJOIN_PID=$!\necho "Join PID: $JOIN_PID"\nsleep 2\nhead -5 /tmp/join.log\n' | prlctl exec "$VM" tee /tmp/launch_join.sh > /dev/null
prlctl exec "$VM" chmod +x /tmp/launch_join.sh
prlctl exec "$VM" /tmp/launch_join.sh

echo "=== 6/6 Restarting viewer ==="
prlctl exec "$VM" sudo killall -9 ndi-viewer 2>/dev/null
sleep 1
printf '#!/bin/bash\nDISPLAY=:0 /home/parallels/ndi-bridge-linux/build/ndi-viewer &\n' | prlctl exec "$VM" tee /tmp/launch_viewer.sh > /dev/null
prlctl exec "$VM" chmod +x /tmp/launch_viewer.sh
prlctl exec "$VM" bash -c "DISPLAY=:0 /tmp/launch_viewer.sh" 2>/dev/null

sleep 2
echo ""
echo "=== Verification ==="
prlctl exec "$VM" head -5 /tmp/join.log
echo ""
prlctl exec "$VM" pgrep -la "ndi-bridge\|ndi-viewer"
echo ""
echo "=== DEPLOY COMPLETE ==="
