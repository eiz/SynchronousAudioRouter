// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Registry data: the redirections of the application-routing filter
//! (section 10, A.11) and the values written for each endpoint interface
//! (section 7, A.10).
#![forbid(unsafe_code)]

use crate::control::utf16_eq_ascii_ignore_case;

/// Altitude the registry callback is registered at.
pub const FILTER_ALTITUDE: &str = "360000";

/// One redirection: reads of `source` are answered from `substitute`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Redirection {
    /// Whether this entry applies to 32-bit (WOW64) processes; the others
    /// apply to native 64-bit processes.
    pub wow64: bool,
    /// Full kernel path of the key whose default value is redirected.
    pub source: &'static str,
    /// Full kernel path of the key that supplies the answer.
    pub substitute: &'static str,
}

macro_rules! inproc_key {
    ($prefix:literal, $clsid:literal) => {
        concat!($prefix, $clsid, "\\InprocServer32")
    };
}

#[cfg(test)]
const NATIVE: &str = "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\";
#[cfg(test)]
const WOW64: &str = "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\";

/// The fixed list of redirections: the audio device enumerator class and the
/// audio interface activation class, each to SAR's wrapper class, for native
/// and for 32-bit processes.
pub const REDIRECTIONS: [Redirection; 4] = [
    Redirection {
        wow64: false,
        source: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\", "{BCDE0395-E52F-467C-8E3D-C4579291692E}"),
        substitute: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\", "{9FB96668-9EDD-4574-AD77-76BD89659D5D}"),
    },
    Redirection {
        wow64: false,
        source: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\", "{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}"),
        substitute: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\", "{739191CC-CCBE-45D8-8D24-828D8E989E8E}"),
    },
    Redirection {
        wow64: true,
        source: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\", "{BCDE0395-E52F-467C-8E3D-C4579291692E}"),
        substitute: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\", "{9FB96668-9EDD-4574-AD77-76BD89659D5D}"),
    },
    Redirection {
        wow64: true,
        source: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\", "{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}"),
        substitute: inproc_key!("\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\", "{739191CC-CCBE-45D8-8D24-828D8E989E8E}"),
    },
];

/// Length in UTF-16 units of the shortest and longest source path; lets the
/// callback discard nearly every key by length alone.
pub fn source_length_bounds() -> (usize, usize) {
    let mut min = usize::MAX;
    let mut max = 0;
    for r in &REDIRECTIONS {
        min = min.min(r.source.len());
        max = max.max(r.source.len());
    }
    (min, max)
}

/// Finds the redirection for a read of `key_path` (the key's full kernel
/// path) by a process of the given bitness. Key names are compared without
/// regard to case.
pub fn find_redirection(key_path: &[u16], wow64_process: bool) -> Option<&'static Redirection> {
    REDIRECTIONS
        .iter()
        .find(|r| r.wow64 == wow64_process && utf16_eq_ascii_ignore_case(key_path, r.source))
}

/// The registry operations the filter acts on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FilteredOperation {
    /// A query of the value with the empty name (the key's default value).
    QueryDefaultValue,
    /// An enumeration of the key's first value (index 0).
    EnumerateFirstValue,
}

/// Classifies a value query by its value name length (in bytes).
pub fn classify_query(value_name_length: usize) -> Option<FilteredOperation> {
    (value_name_length == 0).then_some(FilteredOperation::QueryDefaultValue)
}

/// Classifies a value enumeration by its index.
pub fn classify_enumerate(index: u32) -> Option<FilteredOperation> {
    (index == 0).then_some(FilteredOperation::EnumerateFirstValue)
}

/// `PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY` flag bits (ntddk.h):
/// MicrosoftSignedOnly, StoreSignedOnly, MitigationOptIn.
const SIGNATURE_POLICY_RESTRICTING_BITS: u32 = 0b111;

/// Whether a process with this binary-signature mitigation policy is
/// restricted to Microsoft-signed, Store-signed or similarly restricted
/// images, and therefore could not load the wrapper. The audit-only bits do
/// not restrict anything.
pub fn signature_policy_restricts(flags: u32) -> bool {
    flags & SIGNATURE_POLICY_RESTRICTING_BITS != 0
}

/// Where the data of an endpoint interface's registry value comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValueData {
    /// `REG_SZ`: the endpoint's display name.
    Name,
    /// `REG_SZ`: the endpoint's id exactly as the host gave it.
    Id,
    /// `REG_SZ`: a fixed string.
    Text(&'static str),
    /// `REG_DWORD`.
    Dword(u32),
}

/// One value written under an endpoint interface's parameters key (A.10).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterfaceValue {
    /// Subkey of the parameters key, or `None` for the key itself.
    pub subkey: Option<&'static str>,
    pub name: &'static str,
    pub data: ValueData,
    /// Whether failing to write it fails the endpoint's creation.
    pub required: bool,
}

/// The subkey holding the endpoint property values, as a path of keys to
/// create one after the other.
pub const MSEP_SUBKEY_PATH: [&str; 2] = ["MSEP", "0"];
const MSEP_SUBKEY: &str = "MSEP\\0";

/// The KS proxy class.
pub const KS_PROXY_CLSID: &str = "{17CCA71B-ECD7-11D0-B908-00A0C9223196}";

/// The values written to every interface instance of an endpoint before it
/// is switched on, in the order of A.10.
pub const INTERFACE_VALUES: [InterfaceValue; 6] = [
    InterfaceValue { subkey: None, name: "FriendlyName", data: ValueData::Name, required: true },
    InterfaceValue { subkey: None, name: "CLSID", data: ValueData::Text(KS_PROXY_CLSID), required: true },
    // Event-driven streaming is supported.
    InterfaceValue {
        subkey: Some(MSEP_SUBKEY),
        name: "{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7",
        data: ValueData::Dword(1),
        required: true,
    },
    // No jack association.
    InterfaceValue {
        subkey: Some(MSEP_SUBKEY),
        name: "{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2",
        data: ValueData::Text("{00000000-0000-0000-0000-000000000000}"),
        required: false,
    },
    // The description Windows shows for the endpoint.
    InterfaceValue {
        subkey: Some(MSEP_SUBKEY),
        name: "{a45c254e-df1c-4efd-8020-67d146a850e0},2",
        data: ValueData::Name,
        required: false,
    },
    // SAR's own: the id from the host's configuration.
    InterfaceValue {
        subkey: Some(MSEP_SUBKEY),
        name: "{F4B15B6F-8C3F-48B6-A115-42FDE19EF05B},0",
        data: ValueData::Id,
        required: true,
    },
];

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn table_matches_a11() {
        assert_eq!(
            REDIRECTIONS[0].source,
            "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32"
        );
        assert_eq!(
            REDIRECTIONS[0].substitute,
            "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\{9FB96668-9EDD-4574-AD77-76BD89659D5D}\\InprocServer32"
        );
        assert_eq!(
            REDIRECTIONS[3].source,
            "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}\\InprocServer32"
        );
        assert_eq!(
            REDIRECTIONS[3].substitute,
            "\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\{739191CC-CCBE-45D8-8D24-828D8E989E8E}\\InprocServer32"
        );
        for r in &REDIRECTIONS {
            let prefix = if r.wow64 { WOW64 } else { NATIVE };
            assert!(r.source.starts_with(prefix));
            assert!(r.substitute.starts_with(prefix));
            assert!(r.source.is_ascii() && r.substitute.is_ascii());
            assert_ne!(r.source, r.substitute);
        }
        // A substitute is never itself a source: no redirection loops.
        for a in &REDIRECTIONS {
            for b in &REDIRECTIONS {
                assert!(!a.substitute.eq_ignore_ascii_case(b.source));
            }
        }
    }

    #[test]
    fn lookup_respects_bitness_and_ignores_case() {
        let native = utf16(REDIRECTIONS[0].source);
        let wow = utf16(REDIRECTIONS[2].source);
        assert_eq!(find_redirection(&native, false), Some(&REDIRECTIONS[0]));
        assert_eq!(find_redirection(&native, true), None);
        assert_eq!(find_redirection(&wow, true), Some(&REDIRECTIONS[2]));
        assert_eq!(find_redirection(&wow, false), None);

        let lower = utf16(&REDIRECTIONS[1].source.to_lowercase());
        assert_eq!(find_redirection(&lower, false), Some(&REDIRECTIONS[1]));
        let upper = utf16(&REDIRECTIONS[3].source.to_uppercase());
        assert_eq!(find_redirection(&upper, true), Some(&REDIRECTIONS[3]));
    }

    #[test]
    fn lookup_is_exact() {
        let mut path = utf16(REDIRECTIONS[0].source);
        path.push(u16::from(b'\\'));
        assert_eq!(find_redirection(&path, false), None);
        let parent = utf16(REDIRECTIONS[0].source.trim_end_matches("\\InprocServer32"));
        assert_eq!(find_redirection(&parent, false), None);
        // The wrapper's own key is not redirected.
        assert_eq!(find_redirection(&utf16(REDIRECTIONS[0].substitute), false), None);
        assert_eq!(find_redirection(&[], false), None);
        let (min, max) = source_length_bounds();
        assert!(min <= REDIRECTIONS[0].source.len() && REDIRECTIONS[3].source.len() <= max);
        assert!(min < max);
    }

    #[test]
    fn operations() {
        assert_eq!(classify_query(0), Some(FilteredOperation::QueryDefaultValue));
        assert_eq!(classify_query(2), None);
        assert_eq!(classify_enumerate(0), Some(FilteredOperation::EnumerateFirstValue));
        assert_eq!(classify_enumerate(1), None);
    }

    #[test]
    fn signature_policy() {
        assert!(!signature_policy_restricts(0));
        assert!(signature_policy_restricts(0b001));
        assert!(signature_policy_restricts(0b010));
        assert!(signature_policy_restricts(0b100));
        // Audit bits only.
        assert!(!signature_policy_restricts(0b11000));
    }

    #[test]
    fn interface_values_match_a10() {
        assert_eq!(INTERFACE_VALUES.len(), 6);
        let optional: Vec<&str> = INTERFACE_VALUES.iter().filter(|v| !v.required).map(|v| v.name).collect();
        assert_eq!(
            optional,
            ["{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2", "{a45c254e-df1c-4efd-8020-67d146a850e0},2"]
        );
        assert!(INTERFACE_VALUES[..2].iter().all(|v| v.subkey.is_none()));
        assert!(INTERFACE_VALUES[2..].iter().all(|v| v.subkey == Some("MSEP\\0")));
        assert_eq!(MSEP_SUBKEY_PATH.join("\\"), "MSEP\\0");
        assert_eq!(INTERFACE_VALUES[2].data, ValueData::Dword(1));
        assert_eq!(INTERFACE_VALUES[5].data, ValueData::Id);
    }
}
