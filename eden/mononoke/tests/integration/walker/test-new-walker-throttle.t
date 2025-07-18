# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License found in the LICENSE file in the root
# directory of this source tree.

  $ . "${TEST_FIXTURES}/library.sh"

setup configuration
  $ MULTIPLEXED=1 default_setup_blobimport "blob_files"
  hg repo
  o  C [draft;rev=2;26805aba1e60]
  │
  o  B [draft;rev=1;112478962961]
  │
  o  A [draft;rev=0;426bada5c675]
  $
  blobimporting

Base case, check can walk fine
  $ mononoke_walker -l loaded scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Walked)/s"
  [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43

Check reads throttle by qps
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-read-qps=4 -l loaded scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Walked)/s"
  [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 3 ]]; then echo Took Long Enough Read; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Read

Check reads throttle by bytes
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-bytes-min-throttle=1 --blobstore-read-burst-bytes-s=200 --blobstore-read-bytes-s=200 -l loaded scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Walked)/s"
  [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 4 ]]; then echo Took Long Enough Read; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Read

Check reads throttle by bytes and qps
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-bytes-min-throttle=1 --blobstore-read-burst-bytes-s=200 --blobstore-read-bytes-s=200 --blobstore-read-qps=4 -l loaded scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Walked)/s"
  [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 4 ]]; then echo Took Long Enough Read; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Read

Delete all data from one side of the multiplex
  $ ls blobstore/0/blobs/* | wc -l
  33
  $ rm blobstore/0/blobs/*

Check writes throttle by qps in Repair mode
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-write-qps=4 -l loaded --blobstore-scrub-action=Repair scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Raw|Walked)/s" | sed -re 's/(scrub: blobstore_id BlobstoreId.0. repaired for repo0000.).*/\1/' | uniq -c | sed 's/^ *//'
  1 [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  1 [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  * [INFO] [walker scrub{repo=repo}] scrub: blobstore_id BlobstoreId(0) repaired for repo0000. (glob)
  1 [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 4 ]]; then echo Took Long Enough Repair; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Repair

Check repair happened
  $ ls blobstore/0/blobs/* | wc -l
  30

Delete all data from one side of the multiplex again
  $ rm blobstore/0/blobs/*

Check writes throttle by bytes in Repair mode
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-bytes-min-throttle=1 --blobstore-write-burst-bytes-s=200 --blobstore-write-bytes-s=200 -l loaded --blobstore-scrub-action=Repair scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Raw|Walked)/s" | sed -re 's/(scrub: blobstore_id BlobstoreId.0. repaired for repo0000.).*/\1/' | uniq -c | sed 's/^ *//'
  1 [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  1 [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  30 [INFO] [walker scrub{repo=repo}] scrub: blobstore_id BlobstoreId(0) repaired for repo0000. (glob)
  1 [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 4 ]]; then echo Took Long Enough Repair; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Repair

Check repair happened
  $ ls blobstore/0/blobs/* | wc -l
  30

Delete all data from one side of the multiplex again
  $ rm blobstore/0/blobs/*

Check writes throttle by bytes and qps in Repair mode
  $ START_SECS=$(/bin/date "+%s")
  $ mononoke_walker --blobstore-bytes-min-throttle=1 --blobstore-write-bytes-s=200 --blobstore-read-qps=4 -l loaded --blobstore-scrub-action=Repair scrub -q -I deep -b master_bookmark 2>&1 | grep -vE "(Bytes|Raw|Walked)/s" | sed -re 's/(scrub: blobstore_id BlobstoreId.0. repaired for repo0000.).*/\1/' | uniq -c | sed 's/^ *//'
  1 [INFO] Walking edge types [AliasContentMappingToFileContent, BonsaiHgMappingToHgChangesetViaBonsai, BookmarkToChangeset, ChangesetToBonsaiHgMapping, ChangesetToBonsaiParent, ChangesetToFileContent, FileContentMetadataV2ToGitSha1Alias, FileContentMetadataV2ToSeededBlake3Alias, FileContentMetadataV2ToSha1Alias, FileContentMetadataV2ToSha256Alias, FileContentToFileContentMetadataV2, HgBonsaiMappingToChangeset, HgChangesetToHgManifest, HgChangesetToHgParent, HgChangesetViaBonsaiToHgChangeset, HgFileEnvelopeToFileContent, HgFileNodeToHgCopyfromFileNode, HgFileNodeToHgParentFileNode, HgFileNodeToLinkedHgBonsaiMapping, HgFileNodeToLinkedHgChangeset, HgManifestToChildHgManifest, HgManifestToHgFileEnvelope, HgManifestToHgFileNode]
  1 [INFO] Walking node types [AliasContentMapping, BonsaiHgMapping, Bookmark, Changeset, FileContent, FileContentMetadataV2, HgBonsaiMapping, HgChangeset, HgChangesetViaBonsai, HgFileEnvelope, HgFileNode, HgManifest]
  * [INFO] [walker scrub{repo=repo}] scrub: blobstore_id BlobstoreId(0) repaired for repo0000. (glob)
  1 [INFO] [walker scrub{repo=repo}] Seen,Loaded: 43,43
  $ END_SECS=$(/bin/date "+%s")
  $ ELAPSED_SECS=$(( "$END_SECS" - "$START_SECS" ))
  $ if [[ "$ELAPSED_SECS" -ge 4 ]]; then echo Took Long Enough Repair; else echo "Too short: $ELAPSED_SECS"; fi
  Took Long Enough Repair

Check repair happened
  $ ls blobstore/0/blobs/* | wc -l
  30
