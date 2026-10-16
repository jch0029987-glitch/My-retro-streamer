import 'dart:ffi';
import 'dart:io';
import 'package:flutter/material.dart';

typedef StartHostC = Int32 Function(Uint16 port);
typedef StartHostDart = int Function(int port);

class HostEngineFfi {
  late final DynamicLibrary _lib;
  late final StartHostDart _startHostNode;

  HostEngineFfi() {
    if (Platform.isAndroid) {
      _lib = DynamicLibrary.open("libretro_engine.so");
    } else {
      throw UnsupportedError("Host engine requires Android.");
    }

    _startHostNode = _lib
        .lookup<NativeFunction<StartHostC>>('start_host_node')
        .asFunction<StartHostDart>();
  }

  int startServer(int port) => _startHostNode(port);
}

void main() {
  runApp(const RetroHostApp());
}

class RetroHostApp extends StatelessWidget {
  const RetroHostApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Retro Stream Host',
      theme: ThemeData.dark(useMaterial3: true),
      home: const HostHomePage(),
    );
  }
}

class HostHomePage extends StatefulWidget {
  const HostHomePage({super.key});

  @override
  State<HostHomePage> createState() => _HostHomePageState();
}

class _HostHomePageState extends State<HostHomePage> {
  late final HostEngineFfi _engine;
  bool _isRunning = false;
  String _status = 'Host server is offline';
  final int _port = 8554;

  @override
  void initState() {
    super.initState();
    try {
      _engine = HostEngineFfi();
    } catch (e) {
      _status = 'FFI Load Error: $e';
    }
  }

  void _toggleHost() {
    if (!_isRunning) {
      final res = _engine.startServer(_port);
      setState(() {
        if (res == 0) {
          _isRunning = true;
          _status = 'Hosting on port $_port\nListening for ARMv7-A client inputs...';
        } else {
          _status = 'Failed to start server (Code: $res)';
        }
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Pixel 9 Retro Host Dashboard')),
      body: Padding(
        padding: const EdgeInsets.all(24.0),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Card(
              elevation: 4,
              child: Padding(
                padding: const EdgeInsets.all(32.0),
                child: Column(
                  children: [
                    Icon(
                      _isRunning ? Icons.cell_tower : Icons.power_settings_new,
                      size: 72,
                      color: _isRunning ? Colors.greenAccent : Colors.redAccent,
                    ),
                    const SizedBox(height: 24),
                    Text(
                      _status,
                      textAlign: TextAlign.center,
                      style: const TextStyle(fontSize: 16, height: 1.4),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 32),
            ElevatedButton.icon(
              onPressed: _isRunning ? null : _toggleHost,
              icon: const Icon(Icons.play_arrow),
              label: const Text('Start Host Server', style: TextStyle(fontSize: 16)),
              style: ElevatedButton.styleFrom(
                padding: const EdgeInsets.symmetric(vertical: 16),
                backgroundColor: Colors.blueAccent,
                foregroundColor: Colors.white,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
