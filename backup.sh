#!/bin/bash
# Mirror notes/scripts/WIP to the durable store and push everything to the fork. Safe to run any time.
R=/opt/dlami/nvme/dfperf; D=/home/ubuntu/dfperf-durable
cp -ru $R/notes/. $D/notes/ 2>/dev/null
# small run artefacts (scripts, logs, tables < 512 KiB); raw captures stay on NVMe
mkdir -p $D/runs && cd $R/runs 2>/dev/null && find . -type f -size -512k \( -name '*.sh' -o -name '*.py' -o -name '*.plan' -o -name '*.md' -o -name '*.txt' -o -name '*.tsv' -o -name '*.csv' -o -name '*.log' -o -name '*.json' \) -newer $D/.last_backup -print0 2>/dev/null | cpio -0pdmu --quiet $D/runs 2>/dev/null
# uncommitted work in every worktree, as a patch against its HEAD
mkdir -p $D/patches/wip; cd $R/repo && git worktree list --porcelain | awk '/^worktree/{print $2}' | while read w; do
  n=$(basename $w); (cd $w && { echo "# HEAD $(git rev-parse HEAD) branch $(git branch --show-current)"; git diff HEAD; git ls-files -o --exclude-standard | grep -E '\.(cpp|hpp|cu|cuh|md|sh|py|txt|cmake)$' | head -200 | while read f; do git diff --no-index /dev/null "$f"; done; } > $D/patches/wip/$n.patch 2>/dev/null)
  [ $(wc -l < $D/patches/wip/$n.patch) -le 1 ] && rm -f $D/patches/wip/$n.patch
done
cd $R/repo && for b in $(git for-each-ref --format='%(refname:short)' refs/heads/proto refs/heads/mb refs/heads/prod); do git push -q -f fork "$b:refs/heads/dfperf/$b" 2>&1 | grep -v '^remote:'; done
touch $D/.last_backup
cd $D && git add -A && git -c user.name="Kevin Kristensen" -c user.email="kkristensen@nvidia.com" commit -qm "dfperf checkpoint $(date -u +%F\ %H:%M)" 2>/dev/null; git push -q -f git@github.com:kevkrist/sirius.git HEAD:refs/heads/dfperf/notes 2>&1 | grep -v '^remote:'
echo backup done
