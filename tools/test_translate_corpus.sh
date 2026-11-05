#!/bin/sh
# The translator over the game's own shaders (tools/test_translate_corpus.c): every vertex, fragment and compute stage
# of the RomFS's shader archives translated to GLSL with the switches in the environment (TT_GMEM_MIRROR, TT_GMEM_SSBO,
# TT_STRUCTURED, TT_PUSH, LARECOMP_VK_*), each compiled with glslangValidator (Vulkan 1.3) and checked with spirv-val.
# usage: [TT_...=1] tools/test_translate_corpus.sh <extracted/romfs folder> [name] [jobs]
# The GLSL goes to build/test_corpus/<name>/ (default: "run"); exits 1 when any translation doesn't compile or validate.
# REF=<commit>: also translate with that commit's translate.c into build/test_corpus/<name>-ref/ and compare the two
# byte for byte (a switch that's off must not change the output). Needs a C compiler, glslangValidator, spirv-val.
set -e
cd "$(dirname "$0")/.."
ROMFS=${1:?usage: tools/test_translate_corpus.sh <romfs folder> [name] [jobs]}
NAME=${2:-run}
JOBS=${3:-8}
OUT=build/test_corpus
CC=${CC:-cc}
build_run() {  # <translate.c folder> <out name>
  D="${OUT:?}/${2:?}"
  mkdir -p "$D"
  find "$D" -maxdepth 1 -type f \( -name '*.vert' -o -name '*.frag' -o -name '*.comp' -o -name '*.log' -o -name '*.spv' \) -delete
  $CC -O2 -w -I"$1" tools/test_translate_corpus.c "$1/translate.c" -o "$OUT/test_translate_corpus_$2" -lm
  "$OUT/test_translate_corpus_$2" "$ROMFS" "$D" 2> "$D/stderr.txt" | tee "$D/summary.txt"
}
build_run runtime/nvn/maxwell "$NAME"
D="$OUT/$NAME"
# each file: nothing, or its name (and a .log with the errors)
ls "$D" | grep -E '\.(vert|frag|comp)$' | xargs -P "$JOBS" -I{} sh -c \
  'f="$0/{}"; if glslangValidator -V --target-env vulkan1.3 "$f" -o "$f.spv" > "$f.log" 2>&1 && spirv-val --target-env vulkan1.3 "$f.spv" >> "$f.log" 2>&1; then rm -f -- "$f.log" "$f.spv"; else rm -f -- "$f.spv"; echo "{}"; fi' \
  "$D" > "$D/failed.txt" || true
NF=$(wc -l < "$D/failed.txt")
NT=$(ls "$D" | grep -cE '\.(vert|frag|comp)$')
echo "glslang + spirv-val: $NT translations, $NF failed (logs: $D/<file>.log)" | tee -a "$D/summary.txt"
ND=0
if [ -n "$REF" ]; then
  R="$OUT/ref-src"
  mkdir -p "$R"
  git show "$REF:runtime/nvn/maxwell/translate.c" > "$R/translate.c"
  git show "$REF:runtime/nvn/maxwell/shader.h" > "$R/shader.h"
  # (a reference without the newer switches: this tool's names added as plain variables / fields)
  if ! grep -q maxwell_gmem_ssbo "$R/shader.h"; then
    printf 'bool maxwell_gmem_ssbo;\n' >> "$R/translate.c"
    sed -i 's/^extern bool maxwell_gmem_mirror;/&\nextern bool maxwell_gmem_ssbo;/' "$R/shader.h"
    sed -i 's/^    char error\[256\];.*/&\n    uint16_t gss_mask;/' "$R/shader.h"
  fi
  build_run "$R" "$NAME-ref" > /dev/null
  ND=$(diff -rq -x '*.txt' "$D" "$OUT/$NAME-ref" | wc -l)
  echo "reference $REF: $ND files differ" | tee -a "$D/summary.txt"
fi
[ "$NF" -eq 0 ] && [ "$ND" -eq 0 ]
