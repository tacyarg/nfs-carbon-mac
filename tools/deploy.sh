#!/usr/bin/env bash
# Put the current web build live on nfscarbon.xyz, or roll back.
#
#   tools/deploy.sh                  deploy build/web/recomp + kit/web/player
#   tools/deploy.sh -n               stage and show what would change, then stop
#   tools/deploy.sh rollback v0.4.14 put a rollback-<name> folder back live
#
# Before a release: build, bench, commit and push, and bump the What's new version
# in kit/web/player/index.html. The host is an ssh alias (NFSC_HOST, default
# "nfscarbon") so the origin address behind Cloudflare stays out of this repo.
# The new site is staged next to the live one with unchanged files hardlinked,
# then swapped in; the old one stays whole as rollback-<its version>.
set -euo pipefail
cd "$(dirname "$0")/.."
H=${NFSC_HOST:-nfscarbon}
R=/srv/nfscarbon
P=build/web-site-public/nfsc
W=build/web/recomp/CarbonRecomp.wasm

die() { echo "deploy: $*" >&2; exit 1; }
box() { ssh -o BatchMode=yes "$H" "$@"; }
ver() { grep -o '<summary><b>v[^<]*' | sed 's/.*<b>//'; }
sum() { shasum -a 256 | cut -c1-64; }
fresh() { box "test -e $R/rollback-$1" && echo "$1-$(date +%s)" || echo "$1"; }
live=$(box "cat $R/site/index.html" | ver)

if [ "${1:-}" = rollback ]; then
  v=${2:?usage: tools/deploy.sh rollback <name>, e.g. v0.4.14}
  out=$(fresh "$live")
  # rsync replaces files by rename, so the hardlinked site is never written through.
  box "set -e; cd $R; test -d rollback-$v; rm -rf site.new; cp -al site site.new
       rsync -a rollback-$v/ site.new/; mv site rollback-$out; mv site.new site"
  echo "rolled back to rollback-$v (live now $(box "cat $R/site/index.html" | ver)); undo: tools/deploy.sh rollback $out"
  exit
fi
dry=; [ "${1:-}" = -n ] && dry=1

[ -z "$(git status --porcelain --untracked-files=no)" ] || die "uncommitted changes, commit first"
git merge-base --is-ancestor HEAD fork/online || die "HEAD is not on fork/online, push first"
stale=$(find kit mods game.toml -type f -newer "$W" -not -path 'kit/web/player/*' -not -path '*/tests/*' -not -name '*.md' | head -3)
[ -z "$stale" ] || die "edited since the last build, rebuild first: $stale"
probe=$(strings "$W" | grep PROBE | grep -v D3D9_PROBE_HOTKEY || true)
[ -z "$probe" ] || die "PROBE code in the wasm: $probe"

cp -p build/web/recomp/CarbonRecomp.{wasm,js,data} "$P/"
rsync -a --exclude index.html --exclude tests kit/web/player/ "$P/"
sed -e 's/@APP@/CarbonRecomp/g' -e 's/@TITLE@/Need for Speed: Carbon/g' kit/web/player/index.html > "$P/index.html"
! grep -q '@APP@\|@TITLE@' "$P/index.html" || die "placeholders left in index.html"
new=$(ver < "$P/index.html")
if [ "$new" = "$live" ]; then
  [ -n "$dry" ] || die "$new is already live, bump What's new in kit/web/player/index.html"
  echo "note: $new is already live (a real deploy would stop here)"
fi
ws=$(sum < "$P/CarbonRecomp.wasm")
if [ "$(brotli -dc "$P/CarbonRecomp.wasm.br" 2>/dev/null | sum)" != "$ws" ]; then
  echo "compressing the wasm (brotli -q 11, a minute or two)"
  brotli -q 11 -f -o "$P/CarbonRecomp.wasm.br" "$P/CarbonRecomp.wasm"
  [ "$(brotli -dc "$P/CarbonRecomp.wasm.br" | sum)" = "$ws" ] || die "brotli round trip failed"
fi

box "rm -rf $R/stage"
/usr/bin/rsync -a --exclude .DS_Store --link-dest="$R/site" -e "ssh -o BatchMode=yes" "$P/" "$H:$R/stage/"
echo "staged $new; changed against live ($live):"
box "cd $R && diff -rq site stage | sed 's#^#  #'" || true
if [ -n "$dry" ]; then box "rm -rf $R/stage"; echo "dry run, nothing went live"; exit; fi

out=$(fresh "$live")
box "cd $R && mv site rollback-$out && mv stage site"  # ponytail: two renames, the site is gone for a few ms
sleep 2
got=$(curl -s https://nfscarbon.xyz/ | ver)
lw=$(curl -s --max-time 240 -H 'Accept-Encoding: br' https://nfscarbon.xyz/CarbonRecomp.wasm | brotli -dc | sum)
[ "$got" = "$new" ] && [ "$lw" = "$ws" ] || die "live check failed (index $got, wasm ${lw:0:16}); tools/deploy.sh rollback $out"
echo "$new live $(date -u +%H:%M:%S) UTC: commit $(git rev-parse --short HEAD), wasm sha256 ${ws:0:16}, brotli $(wc -c < "$P/CarbonRecomp.wasm.br" | tr -d ' ') B"
echo "rollback: tools/deploy.sh rollback $out"
