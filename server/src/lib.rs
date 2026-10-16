use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread;
use std::ptr;
use std::ffi::CString;

const MAX_UDP_PACKET_SIZE: usize = 1200;
const TARGET_WIDTH: usize = 640;
const TARGET_HEIGHT: usize = 480;
const EXPECTED_FRAME_SIZE: usize = TARGET_WIDTH * TARGET_HEIGHT * 2; // RGB565
const TOTAL_CHUNKS: usize = 205;

static mut G_FRAME_BUFFER: [u8; EXPECTED_FRAME_SIZE] = [0; EXPECTED_FRAME_SIZE];
static mut G_CHUNKS_BITMAP: [u64; 4] = [0; 4];
static mut G_ACTIVE_SEQUENCE: u32 = 0;

// Thread-safe atomic counters
static G_PACKETS_RECEIVED: AtomicU64 = AtomicU64::new(0);
static G_FRAMES_RENDERED: AtomicU64 = AtomicU64::new(0);

static RUNNING: AtomicBool = AtomicBool::new(false);

// Global JNI state for background callbacks to Kotlin
static mut GLOBAL_JVM: *mut jni_sys::JavaVM = ptr::null_mut();
static mut GLOBAL_RECEIVER_OBJ: jni_sys::jobject = ptr::null_mut();

#[no_mangle]
pub extern "system" fn JNI_OnLoad(vm: *mut jni_sys::JavaVM, _reserved: *mut std::ffi::c_void) -> jni_sys::jint {
    unsafe {
        GLOBAL_JVM = vm;
    }
    jni_sys::JNI_VERSION_1_6
}

unsafe fn push_frame_to_kotlin(env: *mut jni_sys::JNIEnv) {
    if GLOBAL_RECEIVER_OBJ.is_null() || env.is_null() {
        return;
    }
    let env_ptr = *env;
    if env_ptr.is_null() {
        return;
    }
    let functions = *env_ptr;

    let new_byte_array = match functions.NewByteArray {
        Some(f) => f,
        None => return,
    };
    let set_byte_array_region = match functions.SetByteArrayRegion {
        Some(f) => f,
        None => return,
    };
    let get_object_class = match functions.GetObjectClass {
        Some(f) => f,
        None => return,
    };
    let get_method_id = match functions.GetMethodID {
        Some(f) => f,
        None => return,
    };
    let call_void_method = match functions.CallVoidMethod {
        Some(f) => f,
        None => return,
    };

    let byte_array = new_byte_array(env, EXPECTED_FRAME_SIZE as i32);
    if byte_array.is_null() {
        return;
    }

    set_byte_array_region(
        env,
        byte_array,
        0,
        EXPECTED_FRAME_SIZE as i32,
        G_FRAME_BUFFER.as_ptr() as *const i8,
    );

    let clazz = get_object_class(env, GLOBAL_RECEIVER_OBJ);
    if clazz.is_null() {
        return;
    }

    let method_name = match CString::new("onFrameReady") {
        Ok(m) => m,
        Err(_) => return,
    };
    let method_sig = match CString::new("([B)V") {
        Ok(s) => s,
        Err(_) => return,
    };

    let method_id = get_method_id(env, clazz, method_name.as_ptr(), method_sig.as_ptr());
    if method_id.is_null() {
        return;
    }

    // Invoke onFrameReady(frameData) on the Kotlin instance
    call_void_method(env, GLOBAL_RECEIVER_OBJ, method_id, byte_array);
}

#[no_mangle]
pub extern "system" fn Java_com_jeremy_retroclient_NativeReceiver_startReceiver(
    env: *mut jni_sys::JNIEnv,
    thiz: *mut std::ffi::c_void,
    port: i32,
) -> u8 {
    if RUNNING.load(Ordering::SeqCst) {
        return 1;
    }

    unsafe {
        let env_ptr = *env;
        if !env_ptr.is_null() {
            let functions = *env_ptr;
            if let Some(new_global_ref) = functions.NewGlobalRef {
                GLOBAL_RECEIVER_OBJ = new_global_ref(env, thiz);
            }
        }
    }

    let addr = format!("0.0.0.0:{}", port);
    let socket = match UdpSocket::bind(&addr) {
        Ok(s) => s,
        Err(_) => return 0,
    };

    if socket.set_nonblocking(false).is_err() {
        return 0;
    }

    RUNNING.store(true, Ordering::SeqCst);

    thread::spawn(move || {
        let mut packet_buffer = [0u8; MAX_UDP_PACKET_SIZE + 8];

        unsafe {
            G_CHUNKS_BITMAP = [0; 4];
        }

        while RUNNING.load(Ordering::SeqCst) {
            match socket.recv_from(&mut packet_buffer) {
                Ok((n, _)) if n >= 8 => {
                    G_PACKETS_RECEIVED.fetch_add(1, Ordering::Relaxed);

                    let seq = u32::from_be_bytes([
                        packet_buffer[0],
                        packet_buffer[1],
                        packet_buffer[2],
                        packet_buffer[3],
                    ]);
                    let chunk_idx = u16::from_be_bytes([packet_buffer[4], packet_buffer[5]]);

                    let payload_len = n - 8;
                    if chunk_idx as usize >= TOTAL_CHUNKS {
                        continue;
                    }

                    unsafe {
                        if seq != G_ACTIVE_SEQUENCE {
                            G_ACTIVE_SEQUENCE = seq;
                            G_CHUNKS_BITMAP = [0; 4];
                        }

                        let offset = (chunk_idx as usize) * MAX_UDP_PACKET_SIZE;
                        if offset + payload_len <= EXPECTED_FRAME_SIZE {
                            G_FRAME_BUFFER[offset..offset + payload_len]
                                .copy_from_slice(&packet_buffer[8..8 + payload_len]);

                            let word_idx = (chunk_idx / 64) as usize;
                            let bit_idx = (chunk_idx % 64) as usize;
                            G_CHUNKS_BITMAP[word_idx] |= 1u64 << bit_idx;

                            let c0 = G_CHUNKS_BITMAP[0] == 0xFFFFFFFFFFFFFFFF;
                            let c1 = G_CHUNKS_BITMAP[1] == 0xFFFFFFFFFFFFFFFF;
                            let c2 = G_CHUNKS_BITMAP[2] == 0xFFFFFFFFFFFFFFFF;
                            let c3_mask = (1u64 << 13) - 1;
                            let c3 = (G_CHUNKS_BITMAP[3] & c3_mask) == c3_mask;

                            if c0 && c1 && c2 && c3 {
                                G_FRAMES_RENDERED.fetch_add(1, Ordering::Relaxed);

                                // Attach background thread to JVM and push frame to Kotlin UI
                                if !GLOBAL_JVM.is_null() {
                                    let mut env_ptr: *mut jni_sys::JNIEnv = ptr::null_mut();
                                    let vm_ptr = *GLOBAL_JVM;
                                    if !vm_ptr.is_null() {
                                        let vm_functions = *vm_ptr;
                                        if let Some(attach_current_thread) = vm_functions.AttachCurrentThread {
                                            let void_ptr: *mut *mut std::ffi::c_void = &mut env_ptr as *mut *mut jni_sys::JNIEnv as *mut *mut std::ffi::c_void;
                                            attach_current_thread(GLOBAL_JVM, void_ptr, ptr::null_mut());
                                            push_frame_to_kotlin(env_ptr);
                                        }
                                    }
                                }

                                G_CHUNKS_BITMAP = [0; 4];
                            }
                        }
                    }
                }
                _ => {}
            }
        }
    });

    1
}

#[no_mangle]
pub extern "system" fn Java_com_jeremy_retroclient_NativeReceiver_stopReceiver(
    _env: *mut std::ffi::c_void,
    _thiz: *mut std::ffi::c_void,
) {
    RUNNING.store(false, Ordering::SeqCst);
    unsafe {
        GLOBAL_RECEIVER_OBJ = ptr::null_mut();
    }
}

#[no_mangle]
pub extern "system" fn Java_com_jeremy_retroclient_NativeReceiver_getStats(
    env: *mut jni_sys::JNIEnv,
    _thiz: *mut std::ffi::c_void,
) -> jni_sys::jlongArray {
    let packets = G_PACKETS_RECEIVED.load(Ordering::Relaxed) as i64;
    let frames = G_FRAMES_RENDERED.load(Ordering::Relaxed) as i64;

    let stats = [packets, frames];
    unsafe {
        let env_ptr = *env;
        if env_ptr.is_null() {
            return ptr::null_mut();
        }
        let functions = *env_ptr;

        let new_long_array = match functions.NewLongArray {
            Some(f) => f,
            None => return ptr::null_mut(),
        };
        let set_long_array_region = match functions.SetLongArrayRegion {
            Some(f) => f,
            None => return ptr::null_mut(),
        };

        let array = new_long_array(env, 2);
        if array.is_null() {
            return ptr::null_mut();
        }
        set_long_array_region(env, array, 0, 2, stats.as_ptr());
        array
    }
}
