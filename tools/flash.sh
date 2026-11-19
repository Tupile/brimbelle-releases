#!/usr/bin/env bash
# Flash a Brimbelle firmware bundle to the Amped-ESP32 (ESP32-S3) over its
# native USB port. Expects bootloader.bin, partition-table.bin,
# ota_data_initial.bin and brimbelle.bin next to this script.
#
# Usage:
#   ./flash.sh [PORT] [--erase]
#     PORT     serial port (default: auto-detect /dev/ttyACM0 or /dev/ttyUSB0)
#     --erase  wipe the whole flash first (clears Wi-Fi credentials and config)
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=""
ERASE=0
for a in "$@"; do
    case "$a" in
        --erase) ERASE=1 ;;
        *)       PORT="$a" ;;
    esac
done

if [ -z "$PORT" ]; then
    for p in /dev/ttyACM0 /dev/ttyUSB0 /dev/tty.usbmodem* /dev/tty.usbserial*; do
        [ -e "$p" ] && { PORT="$p"; break; }
    done
fi
if [ -z "$PORT" ]; then
    echo "No serial port found. Pass it explicitly, e.g.: ./flash.sh /dev/ttyACM0"
    exit 1
fi

if   command -v esptool.py >/dev/null 2>&1; then ESPTOOL="esptool.py"
elif command -v esptool    >/dev/null 2>&1; then ESPTOOL="esptool"
else ESPTOOL="python3 -m esptool"
fi

for f in bootloader.bin partition-table.bin ota_data_initial.bin brimbelle.bin; do
    [ -f "$DIR/$f" ] || { echo "Missing $f next to this script."; exit 1; }
done

echo "Flashing Brimbelle to $PORT ..."
if [ "$ERASE" = "1" ]; then
    echo "Erasing flash first (Wi-Fi credentials and config will be cleared)..."
    $ESPTOOL --chip esp32s3 -p "$PORT" erase_flash
fi

$ESPTOOL --chip esp32s3 -p "$PORT" -b 460800 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0     "$DIR/bootloader.bin" \
    0x8000  "$DIR/partition-table.bin" \
    0xf000  "$DIR/ota_data_initial.bin" \
    0x20000 "$DIR/brimbelle.bin"

echo
echo "Done. With no Wi-Fi stored the speaker raises the 'Brimbelle-Setup-XXXX'"
echo "hotspot (password printed in the boot log); open http://192.168.4.1/ to set Wi-Fi."
