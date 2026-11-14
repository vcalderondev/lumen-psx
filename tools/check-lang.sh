#!/bin/sh
# Verify that every dist/lang/*.lang has exactly the keys of es.lang, no duplicated keys,
# and every {placeholder} that the Spanish value uses.
#   tools/check-lang.sh [lang dir]      exit 0 = OK, 1 = problems found
cd "$(dirname "$0")/.."
DIR="${1:-dist/lang}"
REF="$DIR/es.lang"
[ -f "$REF" ] || { echo "missing $REF"; exit 1; }
BOM=$(printf '\357\273\277')
TMP="${TMPDIR:-/tmp}/check-lang.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# "key<TAB>value" lines, comments and blank lines removed, whitespace around "=" trimmed
pairs() {
  LC_ALL=C sed -e "1s/^$BOM//" -e '/^[[:space:]]*#/d' -e '/=/!d' \
     -e 's/^[[:space:]]*//' -e 's/[[:space:]]*=[[:space:]]*/	/' -e 's/[[:space:]]*$//' "$1"
}

pairs "$REF" > "$TMP/ref"
cut -f1 "$TMP/ref" | sort > "$TMP/ref.keys"
rc=0
for f in "$DIR"/*.lang; do
  name=$(basename "$f")
  before=$rc; rc=0
  pairs "$f" > "$TMP/cur"
  cut -f1 "$TMP/cur" | sort > "$TMP/cur.keys"
  dups=$(uniq -d "$TMP/cur.keys")
  [ -z "$dups" ] || { echo "$name: duplicated keys: $dups"; rc=1; }
  missing=$(comm -23 "$TMP/ref.keys" "$TMP/cur.keys" | uniq)
  extra=$(comm -13 "$TMP/ref.keys" "$TMP/cur.keys" | uniq)
  [ -z "$missing" ] || { echo "$name: missing keys:"; echo "$missing" | sed 's/^/  /'; rc=1; }
  [ -z "$extra" ] || { echo "$name: keys not in es.lang:"; echo "$extra" | sed 's/^/  /'; rc=1; }
  # placeholders used by the Spanish value must appear in the translation
  while IFS='	' read -r key val; do
    for ph in $(printf '%s\n' "$val" | grep -o '{[a-z_]*}' | sort -u); do
      cur=$(grep -F "$key	" "$TMP/cur" | while IFS='	' read -r k v; do [ "$k" = "$key" ] && printf '%s\n' "$v"; done)
      [ -z "$cur" ] && continue
      case "$cur" in *"$ph"*) ;; *) echo "$name: $key lacks $ph"; rc=1 ;; esac
    done
  done < "$TMP/ref"
  [ $rc -eq 0 ] && echo "ok  $name ($(wc -l < "$TMP/cur.keys" | tr -d ' ') keys)"
  [ $before -eq 0 ] || rc=1
done
exit $rc
