import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

typedef StartClientC = Int32 Function(Pointer<Utf8> hostIp, Uint16 port);
typedef StartClientDart = int Function(Pointer<Utf8> hostIp, int port);

// Optional: FFI signature for sending input state changes
typedef SendInputC = Int32 Function(Uint32 buttonMask);
typedef SendInputDart = int Function(int buttonMask);

class ClientEngineFfi {
  late final DynamicLibrary _lib;
  late final StartClientDart _startClientNode;
  SendInputDart? _sendInput;

  ClientEngineFfi() {
    if (Platform.isAndroid) {
      _lib = DynamicLibrary.open("libretro_engine.so");
    } else {
      throw UnsupportedError("Client engine requires Android.");
    }

    _startClientNode = _lib
        .lookup<NativeFunction<StartClientC>>('start_client_node')
        .asFunction<StartClientDart>();

    try {
      _sendInput = _lib
          .lookup<NativeFunction<SendInputC>>('send_input_state')
          .asFunction<SendInputDart>();
    } catch (_) {
      // Optional binding if implemented in Rust
    }
  }

  int connect(String ip, int port) {
    final ipPtr = ip.toNativeUtf8();
    final res = _startClientNode(ipPtr, port);
    calloc.free(ipPtr);
    return res;
  }

  void pushInput(int mask) {
    _sendInput?.call(mask);
  }
}

void main() {
  runApp(const RetroClientApp());
}

class RetroClientApp extends StatelessWidget {
  const RetroClientApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Retro Stream Client',
      theme: ThemeData.dark(useMaterial3: true),
      home: const ClientHomePage(),
    );
  }
}

class ClientHomePage extends StatefulWidget {
  const ClientHomePage({super.key});

  @override
  State<ClientHomePage> createState() => _ClientHomePageState();
}

class _ClientHomePageState extends State<ClientHomePage> {
  late final ClientEngineFfi _engine;
  final TextEditingController _ipController = TextEditingController();
  bool _isConnected = false;
  String _status = 'Disconnected - Pair Bluetooth Controller';
  final int _port = 8554;
  int _currentButtonMask = 0;

  @override
  void initState() {
    super.initState();
    try {
      _engine = ClientEngineFfi();
    } catch (e) {
      _status = 'FFI Load Error: $e';
    }
  }

  void _connectToHost() {
    final ip = _ipController.text.trim();
    if (ip.isEmpty) return;

    final res = _engine.connect(ip, _port);
    setState(() {
      if (res == 0) {
        _isConnected = true;
        _status = 'Connected to Pixel 9 at $ip:$_port\nListening for Bluetooth Controller...';
      } else {
        _status = 'Connection failed (Code: $res)';
      }
    });
  }

  // Handle hardware gamepad / keyboard events mapped from Bluetooth controller
  KeyEventResult _handleKeyEvent(FocusNode node, KeyEvent event) {
    if (!_isConnected) return KeyEventResult.ignored;

    int maskChange = 0;
    // Map standard Android gamepad keycodes / hardware keys
    if (event.logicalKey == LogicalKeyboardKey.gamepadA ||
        event.logicalKey == LogicalKeyboardKey.select) {
      maskChange = 0x01;
    } else if (event.logicalKey == LogicalKeyboardKey.gamepadB ||
        event.logicalKey == LogicalKeyboardKey.space) {
      maskChange = 0x02;
    } else if (event.logicalKey == LogicalKeyboardKey.gamepadX) {
      maskChange = 0x04;
    } else if (event.logicalKey == LogicalKeyboardKey.gamepadY) {
      maskChange = 0x08;
    }

    if (event is KeyDownEvent) {
      _currentButtonMask |= maskChange;
    } else if (event is KeyUpEvent) {
      _currentButtonMask &= ~maskChange;
    }

    // Stream input state to Rust via FFI
    _engine.pushInput(_currentButtonMask);
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      autofocus: true,
      onKeyEvent: _handleKeyEvent,
      child: Scaffold(
        appBar: AppBar(title: const Text('ARMv7-A Bluetooth Client')),
        body: Padding(
          padding: const EdgeInsets.all(24.0),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Card(
                elevation: 4,
                child: Padding(
                  padding: const EdgeInsets.all(24.0),
                  child: Column(
                    children: [
                      Icon(
                        _isConnected ? Icons.sports_esports : Icons.bluetooth_disabled,
                        size: 64,
                        color: _isConnected ? Colors.blueAccent : Colors.orangeAccent,
                      ),
                      const SizedBox(height: 16),
                      Text(
                        _status,
                        textAlign: TextAlign.center,
                        style: const TextStyle(fontSize: 16, height: 1.4),
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 24),
              TextField(
                controller: _ipController,
                enabled: !_isConnected,
                decoration: const InputDecoration(
                  labelText: 'Pixel 9 Host IP Address',
                  border: OutlineInputBorder(),
                  hintText: 'e.g., 192.168.1.50',
                ),
                keyboardType: TextInputType.number,
              ),
              const SizedBox(height: 20),
              ElevatedButton.icon(
                onPressed: _isConnected ? null : _connectToHost,
                icon: const Icon(Icons.link),
                label: const Text('Connect to Host', style: TextStyle(fontSize: 16)),
                style: ElevatedButton.styleFrom(
                  padding: const EdgeInsets.symmetric(vertical: 16),
                  backgroundColor: Colors.teal,
                  foregroundColor: Colors.white,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
