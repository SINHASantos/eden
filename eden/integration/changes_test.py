#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License version 2.

# pyre-unsafe

import sys
import time
import unittest
from typing import Optional

from facebook.eden.ttypes import (
    Added,
    ChangeNotification,
    ChangesSinceV2Params,
    ChangesSinceV2Result,
    Dtype,
    LargeChangeNotification,
    LostChangesReason,
    Modified,
    Removed,
    Renamed,
    Replaced,
    SmallChangeNotification,
)

from .lib import testcase


def getSmallChangeSafe(
    change: ChangeNotification,
) -> Optional[SmallChangeNotification]:
    if change.getType() == ChangeNotification.SMALLCHANGE:
        return change.get_smallChange()
    return None


def getLargeChangeSafe(
    change: ChangeNotification,
) -> Optional[LargeChangeNotification]:
    if change.getType() == ChangeNotification.LARGECHANGE:
        return change.get_largeChange()
    return None


def buildSmallChange(
    changeType: SmallChangeNotification,
    fileType: Dtype,
    path: Optional[bytes] = None,
    from_path: Optional[bytes] = None,
    to_path: Optional[bytes] = None,
) -> ChangeNotification:
    if changeType == SmallChangeNotification.ADDED:
        assert path
        return ChangeNotification(
            SmallChangeNotification(added=Added(fileType=fileType, path=path))
        )
    elif changeType == SmallChangeNotification.MODIFIED:
        assert path
        return ChangeNotification(
            SmallChangeNotification(modified=Modified(fileType=fileType, path=path))
        )
    elif changeType == SmallChangeNotification.RENAMED:
        assert from_path
        assert to_path
        return ChangeNotification(
            smallChange=SmallChangeNotification(
                renamed=Renamed(
                    fileType=fileType,
                    from_PY_RESERVED_KEYWORD=from_path,
                    to=to_path,
                )
            )
        )
    elif changeType == SmallChangeNotification.REPLACED:
        assert from_path
        assert to_path
        return ChangeNotification(
            smallChange=SmallChangeNotification(
                replaced=Replaced(
                    fileType=Dtype.REGULAR,
                    from_PY_RESERVED_KEYWORD=from_path,
                    to=to_path,
                )
            )
        )

    elif changeType == SmallChangeNotification.REMOVED:
        assert path
        return ChangeNotification(
            SmallChangeNotification(removed=Removed(fileType=fileType, path=path))
        )
    return ChangeNotification()


@testcase.eden_repo_test
class ChangesTest(testcase.EdenRepoTest):
    def populate_repo(self) -> None:
        # Create the initial repo. It requires at least 1 file and 1 commit
        self.repo.write_file("hello", "bonjour\n")
        self.commit0 = self.repo.commit("Commit 0.")

    def setUp(self) -> None:
        # needs to be done before set up because these need to be created
        # for populate_repo() and the supers set up will call this.
        self.commit0 = ""
        self.commit1 = ""

        super().setUp()

        self.client = self.get_thrift_client_legacy()
        self.client.open()
        self.addCleanup(self.client.close)

        self.position = self.client.getCurrentJournalPosition(self.mount_path_bytes)

    def check_changes(self, changes, expected_changes) -> bool:
        expected_changes_index = 0
        for change in changes:
            if change == expected_changes[expected_changes_index]:
                expected_changes_index += 1
                if expected_changes_index == len(expected_changes):
                    return True
        print("Expected changes not found:")
        for i in range(expected_changes_index, len(expected_changes)):
            print(expected_changes[i])
        print("in:")
        print(changes)
        return False

    def getChangesSinceV2(self, position) -> ChangesSinceV2Result:
        return self.client.changesSinceV2(
            ChangesSinceV2Params(
                mountPoint=self.mount_path_bytes, fromPosition=position
            )
        )

    def test_wrong_mount_generation(self):
        # The input mount generation should equal the current mount generation
        oldPosition = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.eden.unmount(self.mount_path)
        self.eden.mount(self.mount_path)
        changes = self.getChangesSinceV2(oldPosition)
        self.assertEqual(len(changes.changes), 1)
        largeChange = getLargeChangeSafe(changes.changes[0])
        self.assertIsNotNone(largeChange)
        self.assertEqual(
            largeChange.get_lostChanges().reason,
            LostChangesReason.EDENFS_REMOUNTED,
        )

    def test_add_file(self):
        position = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.eden_repo.write_file("test_file", "", add=False)
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        changes = self.getChangesSinceV2(position=position)
        # When adding a file, it is technically written to so there's an additional modified operation
        expected_changes = [
            buildSmallChange(
                SmallChangeNotification.ADDED, Dtype.REGULAR, path=b"test_file"
            ),
            buildSmallChange(
                SmallChangeNotification.MODIFIED, Dtype.REGULAR, path=b"test_file"
            ),
        ]
        if sys.platform == "win32":
            # PrjFS notifications only shows up as added, so remove the modified
            expected_changes.pop()
        self.assertTrue(self.check_changes(changes.changes, expected_changes))

    def test_modify_file(self):
        self.eden_repo.write_file("test_file", "", add=False)
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        position = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.eden_repo.write_file("test_file", "contents", add=False)
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        changes = self.getChangesSinceV2(position=position)
        expected_changes = [
            buildSmallChange(
                SmallChangeNotification.MODIFIED, Dtype.REGULAR, path=b"test_file"
            ),
        ]
        self.assertTrue(self.check_changes(changes.changes, expected_changes))

    def test_rename_file(self):
        self.eden_repo.write_file("test_file", "", add=False)
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        position = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.rename("test_file", "best_file")
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        changes = self.getChangesSinceV2(position=position)
        expected_changes = [
            buildSmallChange(
                SmallChangeNotification.RENAMED,
                Dtype.REGULAR,
                from_path=b"test_file",
                to_path=b"best_file",
            ),
        ]
        if sys.platform == "win32":
            # No renames on windows, the files are added/removed
            expected_changes = [
                buildSmallChange(
                    SmallChangeNotification.REMOVED, Dtype.REGULAR, path=b"test_file"
                ),
                buildSmallChange(
                    SmallChangeNotification.ADDED, Dtype.REGULAR, path=b"best_file"
                ),
            ]
        self.assertTrue(self.check_changes(changes.changes, expected_changes))

    @unittest.skipIf(sys.platform == "win32", "PrjFS does not support replace")
    def test_replace_file(self):
        self.eden_repo.write_file("test_file", "test_contents", add=False)
        self.eden_repo.write_file("gone_file", "replaced_contents", add=False)
        position = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.rename("test_file", "gone_file")
        changes = self.getChangesSinceV2(position=position)
        expected_changes = [
            buildSmallChange(
                SmallChangeNotification.REPLACED,
                Dtype.REGULAR,
                from_path=b"test_file",
                to_path=b"gone_file",
            ),
        ]
        self.assertTrue(self.check_changes(changes.changes, expected_changes))

    def test_remove_file(self):
        self.eden_repo.write_file("test_file", "", add=False)
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        position = self.client.getCurrentJournalPosition(self.mount_path_bytes)
        self.rm("test_file")
        if sys.platform == "win32":
            # Wait for eden to get the PrjFS notification
            time.sleep(1)
        changes = self.getChangesSinceV2(position=position)
        expected_changes = [
            buildSmallChange(
                SmallChangeNotification.REMOVED,
                Dtype.REGULAR,
                path=b"test_file",
            ),
        ]
        self.assertTrue(self.check_changes(changes.changes, expected_changes))
