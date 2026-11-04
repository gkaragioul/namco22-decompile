#!/bin/sh
# macos/hd-upscale.sh <hd-dir> <new-dir> -- run by the game at quit (macos/tex_hd.h), detached: AI-upscale the textures the
# session collected in <new-dir> (Real-ESRGAN x4plus) into <hd-dir>, where the next session finds them. One run at a time; the
# originals are kept in <new-dir>.done (to redo them with another model). Does nothing without the upscaler.
HD="$1"; NEW="$2"
# the upscaler: $ESRGAN, else the copy in the project work folder (a development bundle only: the self-contained apps carry a finished pack)
ESR="${ESRGAN:-realesrgan-ncnn-vulkan}"   # Real-ESRGAN (ncnn-vulkan): on the PATH, or ESRGAN=<the program>
[ -x "$ESR" ] && [ -d "$NEW" ] || exit 0
LOCK="$NEW.lock"
mkdir "$LOCK" 2>/dev/null || exit 0
trap 'rmdir "$LOCK" 2>/dev/null' EXIT
IN="$NEW.batch"; OUT="$NEW.out"
mkdir -p "$IN" "$OUT" "$HD" "$NEW.done"
find "$NEW" -maxdepth 1 -name '*.png' -exec mv {} "$IN"/ \;
[ -n "$(find "$IN" -maxdepth 1 -name '*.png' | head -1)" ] || exit 0
# the models by absolute path: the upscaler looks for them relative to the working folder and crashes without them
"$ESR" -i "$IN" -o "$OUT" -n realesrgan-x4plus -m "$(dirname "$ESR")/models" -f png >"$NEW.log" 2>&1
# an original is done only once its HD image exists; anything the upscaler did not finish goes back for the next run
for f in "$IN"/*.png; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    if [ -s "$OUT/$b" ]; then mv -f "$OUT/$b" "$HD/$b"; mv -f "$f" "$NEW.done/$b"; else mv -f "$f" "$NEW/$b"; fi
done
