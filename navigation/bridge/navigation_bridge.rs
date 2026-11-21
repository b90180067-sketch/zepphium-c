unsafe extern "C" {
    fn c_zephium_nav_classify(input: *const u8, input_len: usize, out_url: *mut u8, out_len: usize) -> bool;
    fn c_zephium_nav_is_query(input: *const u8, input_len: usize) -> bool;
    fn c_zephium_nav_is_allowed(url_str: *const u8, len: usize) -> bool;
}

pub fn classify(input: &str) -> Option<String> {
    let mut buf = [0u8; 8192];
    unsafe {
        if c_zephium_nav_classify(input.as_ptr(), input.len(), buf.as_mut_ptr(), buf.len()) {
            let len = buf.iter().position(|&b| b == 0).unwrap_or(0);
            Some(String::from_utf8(buf[..len].to_vec()).unwrap())
        } else {
            None
        }
    }
}

pub fn is_query(input: &str) -> bool {
    unsafe { c_zephium_nav_is_query(input.as_ptr(), input.len()) }
}

pub fn is_allowed(url_str: &str) -> bool {
    unsafe { c_zephium_nav_is_allowed(url_str.as_ptr(), url_str.len()) }
}
