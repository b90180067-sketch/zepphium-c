#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ZephiumId {
    pub bytes: [u8; 16],
}

unsafe extern "C" {
    fn c_zephium_id_parse(s: *const u8, len: usize, out: *mut ZephiumId) -> bool;
    fn c_zephium_id_to_string(id: *const ZephiumId, out_buf: *mut u8, buf_len: usize) -> bool;
    fn c_zephium_id_candidate_after(
        candidate: *const ZephiumId,
        high_water: *const ZephiumId,
        out: *mut ZephiumId,
    ) -> bool;
}

impl ZephiumId {
    pub fn parse(s: &str) -> Option<Self> {
        let mut id = ZephiumId { bytes: [0; 16] };
        unsafe {
            if c_zephium_id_parse(s.as_ptr(), s.len(), &mut id) {
                Some(id)
            } else {
                None
            }
        }
    }

    pub fn to_string(&self) -> String {
        let mut buf = [0u8; 32];
        unsafe {
            c_zephium_id_to_string(self, buf.as_mut_ptr(), buf.len());
        }
        let len = buf.iter().position(|&b| b == 0).unwrap_or(26);
        String::from_utf8(buf[..len].to_vec()).unwrap()
    }

    pub fn candidate_after(candidate: Self, high_water: Option<Self>) -> Option<Self> {
        let mut out = ZephiumId { bytes: [0; 16] };
        let hw_ptr = match &high_water {
            Some(hw) => hw as *const ZephiumId,
            None => std::ptr::null(),
        };
        unsafe {
            if c_zephium_id_candidate_after(&candidate, hw_ptr, &mut out) {
                Some(out)
            } else {
                None
            }
        }
    }
}
