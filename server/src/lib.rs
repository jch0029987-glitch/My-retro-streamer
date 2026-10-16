use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

const MAX_UDP_PACKET_SIZE: usize = 1200;
const TARGET_WIDTH: usize = 640;
const TARGET_HEIGHT: usize = 480;
const EXPECTED_FRAME_SIZE: usize = TARGET_WIDTH * TARGET_HEIGHT * 2; // RGB565
const TOTAL_CHUNKS: usize = 205;

static mut G_FRAME_BUFFER: [u8; EXPECTED_FRAME_SIZE] = [0; EXPECTED_FRAME_SIZE];
static mut G_CHUNKS_BITMAP: [u64; 4] = [0; 4];
static mut G_ACTIVE_SEQUENCE: u32 = 0;
static mut G_PACKETS_RECEIVED: u64 = 0;
static mut G_FRAMES_RENDERED: u64 = 0;

static RUNNING: AtomicBool = AtomicBool::new(false);

#[no_mangle]
pub extern "system" fn Java_com_jeremy_retroclient_NativeReceiver_startReceiver(
    _env: *mut std::ffi::c_void,
    _thiz: *mut std::ffi::c_void,
    port: i32,
) -> u8 {
    if RUNNING.load(Ordering::SeqCst) {
        return 1;
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
                    unsafe {
                        G_PACKETS_RECEIVED += 1;
                    }

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
                                G_FRAMES_RENDERED += 1;
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
}

#[no_mangle]
pub extern "system" fn Java_com_jeremy_retroclient_NativeReceiver_getStats(
    env: *mut jni_sys::JNIEnv,
    _thiz: *mut std::ffi::c_void,
) -> jni_sys::jlongArray {
    let (packets, frames) = unsafe {
        (G_PACKETS_RECEIVED as i64, G_FRAMES_RENDERED as i64)
    };

    let stats = [packets, frames];
    unsafe {
        let env_ref = &mut **env;
        let array = (env_ref.NewLongArray.unwrap())(env, 2);
        if array.is_null() {
            return std::ptr::null_mut();
        }
        (env_ref.SetLongArrayRegion.unwrap())(env, array, 0, 2, stats.as_ptr());
        array
    }
}
