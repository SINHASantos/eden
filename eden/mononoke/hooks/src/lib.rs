/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#![cfg_attr(not(fbcode_build), allow(unused_crate_dependencies))]
#![cfg_attr(test, feature(trait_alias))]
#![feature(let_chains)]

pub mod errors;
#[cfg(fbcode_build)]
mod facebook;
pub mod hook_loader;
mod implementations;
mod lua_pattern;
#[cfg(test)]
mod testlib;

pub use hook_manager::BookmarkHook;
pub use hook_manager::ChangesetHook;
pub use hook_manager::CrossRepoPushSource;
pub use hook_manager::FileHook;
pub use hook_manager::HookExecution;
pub use hook_manager::HookManager;
pub use hook_manager::HookManagerError;
pub use hook_manager::HookOutcome;
pub use hook_manager::HookRejection;
pub use hook_manager::HookRejectionInfo;
pub use hook_manager::HookRepo;
pub use hook_manager::PathContent;
pub use hook_manager::PushAuthoredBy;
pub use implementations::deny_files::DenyFiles;
pub use lua_pattern::LuaPattern;
pub use metaconfig_types::HookConfig;
