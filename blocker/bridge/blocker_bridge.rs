unsafe extern "C" {
    fn c_zephium_blocker_simplify_canonical_host_boundary(
        pattern: *const u8,
        out_buf: *mut u8,
        out_cap: usize,
    ) -> bool;
    fn c_zephium_blocker_split_separator_end(
        pattern: *const u8,
        out_at_end: *mut u8,
        cap_end: usize,
        out_before_sep: *mut u8,
        cap_sep: usize,
    ) -> bool;
}

pub fn simplify_canonical_host_boundary(pattern: &str) -> Option<String> {
    let mut buf = [0u8; 1024];
    let pat_c = std::ffi::CString::new(pattern).ok()?;
    unsafe {
        if c_zephium_blocker_simplify_canonical_host_boundary(
            pat_c.as_ptr() as *const u8,
            buf.as_mut_ptr(),
            buf.len(),
        ) {
            let len = buf.iter().position(|&b| b == 0).unwrap_or(0);
            Some(String::from_utf8(buf[..len].to_vec()).unwrap())
        } else {
            None
        }
    }
}

pub fn split_separator_end(pattern: &str) -> Option<[String; 2]> {
    let mut end_buf = [0u8; 1024];
    let mut sep_buf = [0u8; 1024];
    let pat_c = std::ffi::CString::new(pattern).ok()?;
    unsafe {
        if c_zephium_blocker_split_separator_end(
            pat_c.as_ptr() as *const u8,
            end_buf.as_mut_ptr(),
            end_buf.len(),
            sep_buf.as_mut_ptr(),
            sep_buf.len(),
        ) {
            let end_len = end_buf.iter().position(|&b| b == 0).unwrap_or(0);
            let sep_len = sep_buf.iter().position(|&b| b == 0).unwrap_or(0);
            Some([
                String::from_utf8(end_buf[..end_len].to_vec()).unwrap(),
                String::from_utf8(sep_buf[..sep_len].to_vec()).unwrap(),
            ])
        } else {
            None
        }
    }
}
