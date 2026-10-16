use std::ffi::CStr;
use std::os::raw::c_char;

#[cfg(feature = "host")]
mod host;

#[cfg(feature = "client")]
mod client;

/// Boot up the Pixel 9 Host server session from Flutter
#[no_mangle]
pub extern "C" fn start_host_node(port: u16) -> i32 {
    #[cfg(feature = "host")]
    {
        let addr = format!("0.0.0.0:{}", port);
        std::thread::spawn(move || {
            let rt = tokio::runtime::Runtime::new().unwrap();
            rt.block_on(async {
                if let Err(e) = host::run_host_server(&addr).await {
                    eprintln!("Host error: {}", e);
                }
            });
        });
        0
    }
    #[cfg(not(feature = "host"))]
    {
        -99 // Feature not enabled
    }
}

/// Boot up the ARMv7-A Client session from Flutter
#[no_mangle]
pub extern "C" fn start_client_node(host_ip_ptr: *const c_char, port: u16) -> i32 {
    #[cfg(feature = "client")]
    {
        let c_str = unsafe {
            if host_ip_ptr.is_null() { return -1; }
            CStr::from_ptr(host_ip_ptr)
        };
        
        let host_ip = match c_str.to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return -2,
        };

        let target_addr = format!("{}:{}", host_ip, port);

        std::thread::spawn(move || {
            let rt = tokio::runtime::Runtime::new().unwrap();
            rt.block_on(async {
                if let Err(e) = client::run_client_loop(target_addr).await {
                    eprintln!("Client error: {}", e);
                }
            });
        });
        0
    }
    #[cfg(not(feature = "client"))]
    {
        -99 // Feature not enabled
    }
}
