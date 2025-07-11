/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is dual-licensed under either the MIT license found in the
 * LICENSE-MIT file in the root directory of this source tree or the Apache
 * License, Version 2.0 found in the LICENSE-APACHE file in the root directory
 * of this source tree. You may select, at your option, one of the
 * above-listed licenses.
 */

use std::sync::LazyLock;

#[cfg(fbcode_build)]
mod facebook;
#[cfg(fbcode_build)]
pub use facebook::Env;
#[cfg(fbcode_build)]
pub use facebook::get_env;
#[cfg(fbcode_build)]
pub use facebook::is_corp;
#[cfg(fbcode_build)]
pub use facebook::is_lab;
#[cfg(fbcode_build)]
pub use facebook::is_prod;

pub static IN_PROD: LazyLock<bool> = LazyLock::new(is_prod);
pub static IN_CORP: LazyLock<bool> = LazyLock::new(is_corp);
pub static IN_LAB: LazyLock<bool> = LazyLock::new(is_lab);

#[cfg(not(fbcode_build))]
pub fn get_env() -> u8 {
    0
}

#[cfg(not(fbcode_build))]
pub fn is_prod() -> bool {
    false
}

#[cfg(not(fbcode_build))]
pub fn is_corp() -> bool {
    false
}

#[cfg(not(fbcode_build))]
pub fn is_lab() -> bool {
    false
}

#[unsafe(no_mangle)]
pub extern "C" fn fb_get_env() -> u8 {
    get_env() as u8
}

#[unsafe(no_mangle)]
pub extern "C" fn fb_is_prod() -> bool {
    *IN_PROD
}

#[unsafe(no_mangle)]
pub extern "C" fn fb_is_corp() -> bool {
    *IN_CORP
}

#[unsafe(no_mangle)]
pub extern "C" fn fb_is_lab() -> bool {
    *IN_LAB
}

#[unsafe(no_mangle)]
pub extern "C" fn fb_has_servicerouter() -> bool {
    *IN_PROD
}
