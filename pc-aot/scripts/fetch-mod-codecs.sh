#!/bin/sh
# Fetch the two header-only texture codecs used by MOD import, pinned by
# commit and verified by file hash. Both are public domain / MIT:
#   bcdec  https://github.com/iOrange/bcdec   (BC1-BC7 decoding)
#   stb    https://github.com/nothings/stb    (stb_dxt.h: BC1/BC3 encoding)
set -eu
cd "$(dirname "$0")/.."
fetch() {
  dir=$1 url=$2 rev=$3 file=$4 sum=$5
  if [ ! -d "vendor/$dir/.git" ]; then
    git clone --no-checkout "$url" "vendor/$dir"
  fi
  git -C "vendor/$dir" fetch --depth 1 origin "$rev" 2>/dev/null || git -C "vendor/$dir" fetch origin
  git -C "vendor/$dir" checkout --detach "$rev"
  actual=$(shasum -a 256 "vendor/$dir/$file" 2>/dev/null || sha256sum "vendor/$dir/$file")
  case "$actual" in "$sum"*) ;; *) echo "hash mismatch for vendor/$dir/$file" >&2; exit 1;; esac
}
mkdir -p vendor
fetch bcdec https://github.com/iOrange/bcdec.git 80859ed3b7afb1c527a2a99d70c61457bea72d0c bcdec.h \
  134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189
fetch stb https://github.com/nothings/stb.git 9600522de53973336fa033a289858a4354cbb04c stb_dxt.h \
  8f1a6cc3af7057679a36d32915aff15894521170bcdf95f47c101fdbb1f90500
echo "MOD texture codecs ready"
