 use std::net::SocketAddr;
use std::time::Duration;
use tokio::net::UdpSocket;

pub async fn run_client_loop(host_addr_str: String) -> Result<(), Box<dyn std::error::Error>> {
    let socket = UdpSocket::bind("0.0.0.0:0").await?;
    let host_addr: SocketAddr = host_addr_str.parse()?;
    socket.connect(host_addr).await?;

    println!("Client connected to host at {}", host_addr);

    let mut interval = tokio::time::interval(Duration::from_millis(16)); // ~60Hz polling

    loop {
        interval.tick().await;

        // Sample local gamepad state (provided via FFI or platform channels)
        let sample_button_mask: u32 = 0x0000_0001; 
        let packet = sample_button_mask.to_le_bytes();

        if let Err(e) = socket.send(&packet).await {
            eprintln!("Failed to send input packet: {}", e);
        }
    }
}

