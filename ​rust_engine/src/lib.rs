use std::ffi::CStr;
use std::os::raw::c_char;
use std::net::{UdpSocket, SocketAddr, ToSocketAddrs};
use std::sync::Mutex;

// Thread-safe global storage for the client socket and remote host address
static CLIENT_SOCKET: Mutex<Option<(UdpSocket, SocketAddr)>> = Mutex::new(None);

/// Starts the UDP listener loop on the Pixel 9 Host.
/// Binds to the given port and spawns a background thread to receive client input packets.
#[no_mangle]
pub unsafe extern "C" fn start_host_node(port: u16) -> i32 {
    let addr = format!("0.0.0.0:{}", port);
    let socket = match UdpSocket::bind(&addr) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[Rust Host] Failed to bind socket on {}: {}", addr, e);
            return -1;
        }
    };

    println!("[Rust Host] Successfully listening for UDP inputs on {}", addr);

    // Spawn a lightweight background thread to process incoming input packets continuously
    std::thread::spawn(move || {
        let mut buf = [0u8; 64];
        loop {
            match socket.recv_from(&mut buf) {
                Ok((size, src)) => {
                    if size >= 4 {
                        let button_mask = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
                        println!("[Rust Host] Input received -> Mask: {:#010x} from {}", button_mask, src);
                        // TODO: Feed `button_mask` into your emulator loop / state engine here
                    }
                }
                Err(e) => {
                    eprintln!("[Rust Host] Socket receive error: {}", e);
                    break;
                }
            }
        }
    });

    0
}

/// Connects the ARMv7-A client node to the Pixel 9 host IP over UDP.
#[no_mangle]
pub unsafe extern "C" fn start_client_node(host_ip: *const c_char, port: u16) -> i32 {
    if host_ip.is_null() {
        return -1;
    }

    let c_str = match CStr::from_ptr(host_ip).to_str() {
        Ok(s) => s,
        Err(_) => return -2,
    };

    let target_str = format!("{}:{}", c_str, port);
    let target_addr = match target_str.to_socket_addrs() {
        Ok(mut addrs) => match addrs.next() {
            Some(a) => a,
            None => return -3,
        },
        Err(_) => return -4,
    };

    // Bind local client socket to an ephemeral port
    let socket = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[Rust Client] Failed to bind local client socket: {}", e);
            -5
        }
    };

    // Store socket and destination address in the global Mutex
    if let Ok(mut guard) = CLIENT_SOCKET.lock() {
        *guard = Some((socket, target_addr));
        println!("[Rust Client] Initialized UDP target -> {}", target_str);
        0
    } else {
        -6
    }
}

/// Transmits live controller button bitmasks directly down the UDP data channel.
/// Called from Flutter FFI key event handlers on the ARMv7-A client.
#[no_mangle]
pub extern "C" fn send_input_state(button_mask: u32) -> i32 {
    let guard = match CLIENT_SOCKET.lock() {
        Ok(g) => g,
        Err(_) => return -1,
    };

    let (socket, target) = match &*guard {
        Some(state) => state,
        None => return -2, // Client not connected yet
    };

    let bytes = button_mask.to_le_bytes();
    match socket.send_to(&bytes, target) {
        Ok(_) => 0,
        Err(e) => {
            eprintln!("[Rust Client] Failed to transmit input packet: {}", e);
            -3
        }
    }
}
