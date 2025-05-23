/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

use percent_encoding::AsciiSet;
use percent_encoding::CONTROLS;
use percent_encoding::utf8_percent_encode;

// https://url.spec.whatwg.org/#fragment-percent-encode-set
const FRAGMENT: &AsciiSet = &CONTROLS.add(b' ').add(b'"').add(b'<').add(b'>').add(b'`');
// https://url.spec.whatwg.org/#path-percent-encode-set
const PATH: &AsciiSet = &FRAGMENT.add(b'#').add(b'?').add(b'{').add(b'}');
const USERINFO: &AsciiSet = &PATH
    .add(b'/')
    .add(b':')
    .add(b';')
    .add(b'=')
    .add(b'@')
    .add(b'[')
    .add(b'\\')
    .add(b']')
    .add(b'^')
    .add(b'|');

pub const HG_ENCODE_SET: &AsciiSet = &USERINFO.add(b',');

pub fn percent_encode(input: &str) -> String {
    // This encode set doesn't exactly match what Python's urllib does, but it's
    // close enough and importantly it encodes '=' which is the only important
    // one.
    utf8_percent_encode(input, HG_ENCODE_SET).collect::<String>()
}
