#!/bin/bash
# Build and run the host unit tests. No ESP-IDF or hardware needed.
# These cover the pure-logic modules (no esp_* / FreeRTOS dependencies).
set -e
cd "$(dirname "$0")"
OUT=build
mkdir -p "$OUT"

echo "=== building audio_mix host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/audio/include \
    -o "$OUT/test_audio_mix" \
    test_audio_mix.c ../../components/audio/audio_mix.c -lm

echo "=== running ==="
"$OUT/test_audio_mix"

echo "=== building audio_eq host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/audio/include \
    -o "$OUT/test_audio_eq" \
    test_audio_eq.c ../../components/audio/audio_eq.c -lm

echo "=== running ==="
"$OUT/test_audio_eq"

echo "=== building url_escape host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/http_util/include \
    -o "$OUT/test_url_escape" \
    test_url_escape.c ../../components/http_util/url_escape.c

echo "=== running ==="
"$OUT/test_url_escape"

echo "=== building http_util host tests ==="
# Single-TU build: the test includes http_util.c to share the scripted
# esp_http_client stub state.
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/http_util/include \
    -o "$OUT/test_http_util" \
    test_http_util.c ../../components/http_util/url_escape.c

echo "=== running ==="
"$OUT/test_http_util"

echo "=== building adts host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/decode \
    -o "$OUT/test_adts" \
    test_adts.c ../../components/decode/adts.c

echo "=== running ==="
"$OUT/test_adts"

echo "=== building tags host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/decode/include \
    -o "$OUT/test_tags" \
    test_tags.c ../../components/decode/tags.c

echo "=== running ==="
"$OUT/test_tags"

echo "=== building rf_meta host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/source_stream \
    -o "$OUT/test_rf_meta" \
    test_rf_meta.c ../../components/source_stream/rf_meta.c

echo "=== running ==="
"$OUT/test_rf_meta"

echo "=== building buttons host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/encoder/include \
    -o "$OUT/test_buttons" \
    test_buttons.c ../../components/encoder/buttons.c

echo "=== running ==="
"$OUT/test_buttons"

echo "=== building enc_logic host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/encoder/include \
    -o "$OUT/test_enc_logic" \
    test_enc_logic.c ../../components/encoder/enc_logic.c

echo "=== running ==="
"$OUT/test_enc_logic"
