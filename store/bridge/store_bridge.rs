pub enum ZephiumStoreOpaque {}

unsafe extern "C" {
    fn c_zephium_store_open(path: *const u8, out_store: *mut *mut ZephiumStoreOpaque) -> bool;
    fn c_zephium_store_close(store: *mut ZephiumStoreOpaque);
    fn c_zephium_store_add_bookmark(
        store: *mut ZephiumStoreOpaque,
        id: *const u8,
        url: *const u8,
        title: *const u8,
    ) -> bool;
    fn c_zephium_store_count_bookmarks(store: *mut ZephiumStoreOpaque) -> usize;
}

pub struct Store {
    ptr: *mut ZephiumStoreOpaque,
}

impl Store {
    pub fn open(path: &str) -> Option<Self> {
        let path_c = std::ffi::CString::new(path).ok()?;
        let mut ptr = std::ptr::null_mut();
        unsafe {
            if c_zephium_store_open(path_c.as_ptr() as *const u8, &mut ptr) {
                Some(Store { ptr })
            } else {
                None
            }
        }
    }

    pub fn count_bookmarks(&self) -> usize {
        unsafe { c_zephium_store_count_bookmarks(self.ptr) }
    }
}

impl Drop for Store {
    fn drop(&mut self) {
        unsafe { c_zephium_store_close(self.ptr) }
    }
}
