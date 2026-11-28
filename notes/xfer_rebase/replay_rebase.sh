#!/bin/bash
# Replays "rebase the construction stack onto the transfer PR" and records every conflict.
# Usage: replay_rebase.sh <stack head (no transfer)> <stack base> <transfer head> <resolved stack head> <log>
# Each picked commit is resolved by taking the conflicted files from the commit with the same subject on <resolved>.
set -u
R=/opt/dlami/nvme/dfperf; src=$1; base=$2; onto=$3; res=$4; log=$5
cd $R/repo; git worktree remove --force $R/wt-replay 2>/dev/null; git worktree add -q --detach $R/wt-replay $onto; cd $R/wt-replay
: > $log
for c in $(git rev-list --reverse $base..$src); do
  subj=$(git log -1 --format=%s $c)
  tgt=$(git log --format='%H %s' $onto..$res | awk -v s="$subj" '{h=$1; $1=""; if (substr($0,2)==s) print h}')
  if git cherry-pick $c >/dev/null 2>&1; then st=clean; else
    st=CONFLICT
    for f in $(git diff --name-only --diff-filter=U); do echo "  conflict: $f" >> $log.tmp; git checkout $tgt -- "$f" 2>/dev/null || git rm -q "$f"; done
    git -c core.editor=true cherry-pick --continue >/dev/null 2>&1
  fi
  same=$([ "$(git rev-parse HEAD^{tree})" = "$(git rev-parse $tgt^{tree})" ] && echo tree-equal || echo "TREE-DIFF: $(git diff --name-only HEAD $tgt | tr '\n' ' ')")
  echo "$(git rev-parse --short $c) -> $(git rev-parse --short $tgt) $st $same | $subj" >> $log
  [ -f $log.tmp ] && { cat $log.tmp >> $log; rm $log.tmp; }
  # carry forward the resolved tree so that later picks start from the recorded state
  [ "$same" = tree-equal ] || { git checkout -q $tgt -- . ; git diff --cached --quiet || git commit -qm "SEMANTIC (non-conflict) delta after: $subj"; }
done
