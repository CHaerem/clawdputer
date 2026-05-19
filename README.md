# wokwi-firmware

Auto-generated orphan branch. Force-pushed by the build job in
`.github/workflows/firmware.yml` on every main push. Carries
firmware/wokwi.bin (merged bootloader + partitions + app, built
from the cardputer-wokwi PIO env which disables peripherals the
simulator can't ACK) plus the compiled keyboard chip wasm and a
wokwi.toml that ties them together. The "Open in Wokwi" badge
on the web demo points here.
