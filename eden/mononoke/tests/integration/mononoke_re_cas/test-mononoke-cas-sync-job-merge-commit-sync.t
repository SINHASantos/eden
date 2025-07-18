# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License found in the LICENSE file in the root
# directory of this source tree.

  $ . "${TEST_FIXTURES}/library.sh"
  $ setup_common_config
  $ export CAS_STORE_PATH="$TESTTMP"
  $ setconfig drawdag.defaultfiles=false

  $ start_and_wait_for_mononoke_server
  $ hg clone -q mono:repo repo
  $ cd repo
  $ drawdag --parent-order=name << 'EOF'
  > F        # F/quux = random:30
  > |\       # D/qux  = random:30
  > B D      # C/baz  = random:30
  > | |      # B/bar  = random:30
  > A C      # A/foo  = random:30
  > EOF

  $ hg log -r "p1($F)" -T "{node}" | grep $D > /dev/null
  [1]
  $ hg log -r "p2($F)" -T "{node}" | grep $D > /dev/null
  $ hg push -r $B --to master_bookmark -q --create
  $ hg push -r $D --allow-anon -q
  $ hg push -r $F --to master_bookmark -q
  $ hg goto $F -q
  $ ls | sort
  bar
  baz
  foo
  quux
  qux

Sync all bookmarks moves (the second move is a merge commit)
  $ mononoke_cas_sync repo 0
  [INFO] [execute{repo=repo}] Initiating mononoke RE CAS sync command execution
  [INFO] [execute{repo=repo}] using repo "repo" repoid RepositoryId(0)
  [INFO] [execute{repo=repo}] syncing log entries [1, 2] ...
  [INFO] [execute{repo=repo}] log entry BookmarkUpdateLogEntry * is a creation of bookmark (glob)
  [INFO] [execute{repo=repo}] log entries [1, 2] synced (3 commits uploaded, upload stats: uploaded digests: 8, already present digests: 0, uploaded bytes: 1.6 KiB, the largest uploaded blob: 914 B), took overall * sec (glob)
  [INFO] [execute{repo=repo}] queue size after processing: 0
  [INFO] [execute{repo=repo}] successful sync of entries [1, 2]
  [INFO] [execute{repo=repo}] Finished mononoke RE CAS sync command execution for repo repo

Verify that all the blobs are in CAS for the merge commit F
  $ mononoke_admin cas-store --repo-name repo upload --full -i $F
  [INFO] Upload completed. Upload stats: uploaded digests: 0, already present digests: 6, uploaded bytes: 0 B, the largest uploaded blob: 0 B
