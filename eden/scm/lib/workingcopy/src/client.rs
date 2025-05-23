/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::any::Any;
use std::collections::BTreeMap;

use anyhow::Result;
use anyhow::bail;
use gitcompat::GitCmd;
use gitcompat::rungit::RepoGit;
use types::HgId;
use types::RepoPathBuf;
use types::hgid::GIT_EMPTY_TREE_ID;
use types::workingcopy_client::CheckoutConflict;
use types::workingcopy_client::CheckoutMode;
use types::workingcopy_client::FileStatus;
use types::workingcopy_client::ProgressInfo;

/// The "client" that talks to an external program for working copy management.
/// Practically, a "client" could be an "edenfs" client or a "git" client.
///
/// This trait is initially modeled after edenfs-client. It does not keep states
/// about treestate or write to the filesystem. The treestate/dirstate related
/// logic lives at a higher level.
pub trait WorkingCopyClient: Send + Sync {
    /// Get the "status". Note: those "status" are from the "external" client,
    /// they are probably not the final "status" and needs furtuer processing.
    fn get_status(
        &self,
        node: HgId,
        list_ignored: bool,
    ) -> Result<BTreeMap<RepoPathBuf, FileStatus>>;

    /// Set parents (tracked by the external program) without changing the working copy content.
    /// This is used by commands like `reset -k`.
    fn set_parents(&self, p1: HgId, p2: Option<HgId>, p1_tree: HgId) -> Result<()>;

    /// Progress as reported by the external program (in reported units of
    /// progress, not percentage).
    /// When a checkout is not ongoing it returns None.
    fn checkout_progress(&self) -> Result<Option<ProgressInfo>>;

    /// Checkout. Set parents and update working copy content.
    fn checkout(
        &self,
        node: HgId,
        tree_node: HgId,
        mode: CheckoutMode,
    ) -> Result<Vec<CheckoutConflict>>;

    /// For downcast.
    fn as_any(&self) -> &dyn Any;
}

#[cfg(feature = "eden")]
impl WorkingCopyClient for edenfs_client::EdenFsClient {
    fn get_status(
        &self,
        node: HgId,
        list_ignored: bool,
    ) -> Result<BTreeMap<RepoPathBuf, FileStatus>> {
        tracing::debug!(p1=?node, list_ignored=list_ignored, "get_status");
        edenfs_client::EdenFsClient::get_status(self, node, list_ignored)
    }

    fn set_parents(&self, p1: HgId, p2: Option<HgId>, p1_tree: HgId) -> Result<()> {
        tracing::debug!(p1=?p1, p2=?p2, p1_tree=?p1_tree, "set_parents");
        edenfs_client::EdenFsClient::set_parents(self, p1, p2, p1_tree)
    }

    fn checkout(
        &self,
        node: HgId,
        tree_node: HgId,
        mode: edenfs_client::CheckoutMode,
    ) -> Result<Vec<CheckoutConflict>> {
        tracing::debug!(p1=?node, p1_tree=?tree_node, mode=?mode, "checkout");
        edenfs_client::EdenFsClient::checkout(self, node, tree_node, mode)
    }

    fn checkout_progress(&self) -> Result<Option<ProgressInfo>> {
        edenfs_client::EdenFsClient::checkout_progress(self)
    }

    fn as_any(&self) -> &dyn Any {
        self as &dyn Any
    }
}

impl WorkingCopyClient for RepoGit {
    fn get_status(
        &self,
        node: HgId,
        list_ignored: bool,
    ) -> Result<BTreeMap<RepoPathBuf, FileStatus>> {
        tracing::debug!(node=?node, list_ignored=list_ignored, "get_status");
        let args = [
            "--no-optional-locks",
            "--porcelain=1",
            "--ignore-submodules=dirty",
            "--untracked-files=all",
            "--no-renames",
            "-z",
            if list_ignored {
                "--ignored"
            } else {
                "--ignored=no"
            },
        ];
        let out = self.call("status", &args)?;

        // Example output:
        // AD file/path   (added to index, deleted on disk)
        // See https://github.com/git/git/blob/2162f9f6f86df4f49c3a716b5beb3952104ea8b8/Documentation/git-status.txt#L218-L244

        let changes = out
            .stdout
            .split(|&c| c == 0)
            .filter_map(|line| {
                if line.get(2) != Some(&b' ') {
                    // Unknown format. Ignore.
                    return None;
                }
                let path_bytes = line.get(3..)?;
                let path = RepoPathBuf::from_utf8(path_bytes.to_vec()).ok()?;
                let (mut x, y) = (line[0] /* index */, line[1] /* working copy */);
                // If deleted in working copy, consider as deletion.
                if y == b'D' {
                    x = y
                };
                let status = match x {
                    b'D' => FileStatus::Removed,
                    b'A' | b'?' => FileStatus::Added,
                    b'!' => FileStatus::Ignored,
                    _ => FileStatus::Modified,
                };
                Some((path, status))
            })
            .collect();
        Ok(changes)
    }

    fn set_parents(&self, p1: HgId, p2: Option<HgId>, mut p1_tree: HgId) -> Result<()> {
        tracing::debug!(?p1, ?p2, ?p1_tree, "set_parents");
        // TODO: What to do with p2?
        if self.resolve_head()? != p1 {
            let p1_hex = p1.to_hex();
            self.call("update-ref", &["HEAD", &p1_hex])?;
            if !p1_tree.is_wdir() {
                if p1_tree.is_null() {
                    p1_tree = GIT_EMPTY_TREE_ID;
                }
                let p1_tree_hex = p1_tree.to_hex();
                self.call("read-tree", &["--no-recurse-submodules", &p1_tree_hex])?;
            }
        }
        Ok(())
    }

    fn checkout(
        &self,
        node: HgId,
        tree_node: HgId,
        mode: CheckoutMode,
    ) -> Result<Vec<CheckoutConflict>> {
        tracing::debug!(p1=?node, p1_tree=?tree_node, mode=?mode, "checkout");
        // TODO: Conflicts are not reported properly. Are they needed?
        let flags = match mode {
            CheckoutMode::Normal => "-d",
            CheckoutMode::Force => "-fd",
            CheckoutMode::DryRun => return Ok(Vec::new()),
        };

        let hex = node.to_hex();
        self.call("checkout", &[flags, "--recurse-submodules", &hex])?;

        Ok(Vec::new())
    }

    fn as_any(&self) -> &dyn Any {
        self as &dyn Any
    }

    fn checkout_progress(&self) -> Result<Option<ProgressInfo>> {
        bail!("Progress for Git checkout not yet implemented!");
    }
}
