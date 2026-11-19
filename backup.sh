#!/bin/bash
# Mirror notes/scripts to the durable store and push everything to the fork. Safe to run any time.
R=/opt/dlami/nvme/dfperf; D=/home/ubuntu/dfperf-durable
cp -ru $R/notes/. $D/notes/ 2>/dev/null
cd $R/repo && for b in $(git for-each-ref --format='%(refname:short)' refs/heads/proto refs/heads/mb refs/heads/prod); do git push -q -f fork "$b:refs/heads/dfperf/$b" 2>&1 | grep -v '^remote:'; done
cd $D && git add -A && git -c user.name="Kevin Kristensen" -c user.email="kkristensen@nvidia.com" commit -qm "dfperf backup" 2>/dev/null; git push -q -f git@github.com:kevkrist/sirius.git HEAD:refs/heads/dfperf/notes 2>&1 | grep -v '^remote:'
echo backup done
